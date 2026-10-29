"""Fornaio AI worker, protocol v1. Dependencies are optional and loaded only on demand.

Generation uses Fibo 2.0 Edit Turbo: each request edits a camera-lit clay render of the mesh (or the
accepted paint seen from the camera, with clay on the unpainted surface) into the requested material.
Production runs with a private, offline Python runtime. --fake needs only the standard library and is
exclusively for deterministic integration tests.
"""
import argparse
import gc
import json
import os
import queue
import shutil
import sys
import threading
import time
import traceback

os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["TRANSFORMERS_OFFLINE"] = "1"
os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
os.environ["DO_NOT_TRACK"] = "1"
PROTOCOL = 1
STEPS = 4
MAX_REFERENCES = 3  # Fibo edits at most 4 images: the camera guide plus 3 appearance references.
commands = queue.Queue()
cancelled = threading.Event()
output_lock = threading.Lock()


def fp8_linear_class():
    import torch

    class Fp8Linear(torch.nn.Module):
        """E4M3 weights with a per-tensor scale and dynamic activation scale, for Ada/Hopper tensor cores."""
        def __init__(self, linear):
            super().__init__()
            weight = linear.weight.detach().float()
            scale = (weight.abs().amax().clamp(min=1e-12) / 448).reshape(())
            # Buffers follow the module between devices under CPU offloading.
            self.register_buffer("weight_fp8", (weight / scale).to(torch.float8_e4m3fn))
            self.register_buffer("weight_scale", scale)
            self.register_buffer("bias", None if linear.bias is None else linear.bias.detach().to(torch.bfloat16))
            self.out_features = linear.out_features

        def forward(self, x):
            shape = x.shape
            x2 = x.reshape(-1, shape[-1]).to(torch.bfloat16)
            x_scale = (x2.abs().amax().float().clamp(min=1e-12) / 448).reshape(())
            xq = (x2 / x_scale.to(x2.dtype)).to(torch.float8_e4m3fn)
            y = torch._scaled_mm(xq, self.weight_fp8.t(), scale_a=x_scale, scale_b=self.weight_scale,
                                 bias=self.bias, out_dtype=torch.bfloat16)
            return y.reshape(*shape[:-1], self.out_features)
    return Fp8Linear


def quantize_fp8(module, device, minimum=1024):
    """Replace large linear layers in place; each is quantized on the GPU, then returned to its device."""
    import torch
    Fp8Linear = fp8_linear_class()
    for name, child in list(module.named_children()):
        if isinstance(child, torch.nn.Linear) and min(child.in_features, child.out_features) >= minimum \
                and child.in_features % 16 == 0 and child.out_features % 16 == 0:
            home = child.weight.device
            setattr(module, name, Fp8Linear(child.to(device)).to(home))
            del child
        else:
            quantize_fp8(child, device, minimum)


def image_only_processor(model):
    """Qwen3-VL processor without its video processor (which needs torchvision); Fibo only sends images."""
    from transformers import AutoTokenizer, Qwen2VLImageProcessor, Qwen3VLProcessor
    with open(os.path.join(model, "processor", "processor_config.json"), encoding="utf-8") as f:
        config = json.load(f)["image_processor"]
    config.pop("image_processor_type", None)
    tokenizer = AutoTokenizer.from_pretrained(os.path.join(model, "processor"))
    processor = Qwen3VLProcessor.__new__(Qwen3VLProcessor)
    processor.image_processor, processor.tokenizer, processor.video_processor = \
        Qwen2VLImageProcessor(**config), tokenizer, None
    processor.chat_template = getattr(tokenizer, "chat_template", None)
    for attr, token in (("image_token", "<|image_pad|>"), ("video_token", "<|video_pad|>"),
                        ("vision_start_token", "<|vision_start|>"), ("vision_end_token", "<|vision_end|>")):
        setattr(processor, attr, token)
        setattr(processor, attr + "_id", tokenizer.convert_tokens_to_ids(token))
    return processor


def linear_patch_embed(patch_embed):
    """Qwen3-VL's stride==kernel Conv3d is a patchify matmul. cuDNN's bf16 Conv3d takes ~15 s here on Windows."""
    import torch
    conv = patch_embed.proj

    def forward(hidden_states):
        weight = conv.weight.reshape(conv.out_channels, -1)
        x = hidden_states.to(weight.dtype).reshape(-1, weight.shape[1])
        return torch.nn.functional.linear(x, weight, conv.bias).view(-1, conv.out_channels)
    patch_embed.forward = forward


def clay(normal, silhouette, low=.25, high=.6):
    """Camera-lit mid-gray clay: shape without a directional shadow to copy, never mistaken for the white
    background (near-white clay was left unpainted and projected as white)."""
    import numpy as np
    from PIL import Image
    v = np.asarray(normal.convert("RGB"), np.float32) / 127.5 - 1
    v /= np.maximum(np.linalg.norm(v, axis=2, keepdims=True), 1e-6)
    shade = low + (high - low) * np.clip(v[..., 2], 0, 1)
    if silhouette.size != normal.size:
        raise ValueError("Normal guide and silhouette dimensions do not match")
    shade[np.asarray(silhouette.convert("L")) <= 127] = 1
    return Image.fromarray(np.rint(shade * 255).astype(np.uint8)).convert("RGB")


def guide_image(command):
    """Fresh views edit clay. Later views keep the app's reliable paint and show clay on every other surface."""
    import numpy as np
    from PIL import Image
    with Image.open(command["normal"]) as n, Image.open(command["silhouette"]) as s:
        base = clay(n, s)
        silhouette = np.asarray(s.convert("L")) > 127
    if command.get("conditioning") != "confidence":
        return base
    with Image.open(command["feedback"]) as f, Image.open(command["feedbackMask"]) as m:
        paint = np.asarray(f.convert("RGB"))
        reuse = np.asarray(m.convert("L")) > 0
    if paint.shape[:2] != silhouette.shape:
        raise ValueError("Paint guide does not match the camera")
    # The app's feedback image shows paint as context even where it is not reused; flat neutral is unpainted.
    painted = (reuse | ~np.all(paint == 150, axis=2)) & silhouette
    return Image.fromarray(np.where(painted[..., None], paint, np.asarray(base)).astype(np.uint8))


def load_references(command):
    from PIL import Image, ImageOps
    images = []
    for path in command.get("references", [])[:MAX_REFERENCES]:
        with Image.open(path) as source:
            # Hidden RGB under transparent pixels must never become material color.
            rgba = ImageOps.exif_transpose(source).convert("RGBA")
        image = Image.new("RGB", rgba.size, "white")
        image.paste(rgba, mask=rgba.getchannel("A"))
        image.thumbnail((768, 768), Image.Resampling.LANCZOS)
        images.append(image)
    return images


def view_description(command):
    """Name the camera relative to the first (front) view, so front features in a reference are not painted
    on the back. Purely geometric: nothing here assumes what the object or the references show."""
    turn, pitch = command.get("viewTurn"), command.get("viewPitch", 0)
    if turn is None:
        return "identical to <image_1>"
    if pitch >= 50:
        return "top-down view from above, identical to <image_1>; the front of the object is at the lower edge"
    if pitch <= -50:
        return "view from below, identical to <image_1>; the front of the object is at the upper edge"
    if turn >= 110:
        return ("rear view, identical to <image_1>: the camera looks at the back of the object; its front is on the "
                "hidden far side, so features that belong on the front are not visible")
    if turn >= 45:
        return "side view, identical to <image_1>; front features appear only where <image_1> shows the front"
    return "front view, identical to <image_1>"


DESCRIBE_REFERENCE = (
    "Describe the visual appearance in this image so it can be painted onto a 3D model: what it is, its colors, "
    "materials, patterns, markings and fine surface details. Do not describe lighting, shadows, the background or "
    "the camera. Answer in one paragraph of at most 70 words.")


def describe_reference(pipe, image):
    """Fibo's own text encoder (Qwen3-VL-4B-Instruct) reads the reference and writes its appearance description.

    Fibo is trained on detailed captions; a caption that does not describe the reference gives generic output.
    """
    import torch
    processor = pipe.processor
    text = ("<|im_start|>user\n<|vision_start|><|image_pad|><|vision_end|>" + DESCRIBE_REFERENCE +
            "<|im_end|>\n<|im_start|>assistant\n")
    inputs = processor(text=[text], images=[image], return_tensors="pt").to(pipe.text_encoder.device)
    with torch.inference_mode():
        output = pipe.text_encoder.generate(**inputs, max_new_tokens=120, do_sample=False)
    answer = processor.tokenizer.decode(output[0, inputs["input_ids"].shape[1]:], skip_special_tokens=True)
    return " ".join(answer.split())


def generation_caption(command, descriptions):
    """Structured (VGL) caption. Only the rules are fixed: keep the geometry and camera, flat shadowless lighting,
    white background. The appearance comes from the user's prompt and from the reference descriptions."""
    prompt = (command.get("prompt") or "").strip().rstrip(".")
    slots = [f"<image_{index + 2}>" for index in range(len(descriptions))]
    shown = " and ".join(slots)
    described = " ".join(f"{slot}: {text.rstrip('.')}." for slot, text in zip(slots, descriptions))
    if prompt and slots:
        look = f"{prompt}, as shown in {shown}"
    elif slots:
        look = f"the appearance shown in {shown}"
    else:
        look = prompt or "a plausible appearance"
    details = " ".join(part for part in (f"{prompt}." if prompt else "", described) if part)
    if command.get("conditioning") == "confidence":
        instruction = ("Complete the texturing of the partially painted object in <image_1>: keep every painted "
                       "area exactly as it is, and paint the bare gray clay areas to match it, continuing its "
                       "patterns, colors and detail scale seamlessly across the boundary"
                       + (f", consistent with {shown}" if slots else "") +
                       ". Keep the exact shape, silhouette and camera, with flat shadowless lighting.")
    else:
        # The mesh is fixed: a reference of another kind of object (an animal, a logo) must be painted onto
        # this surface, never turn the object into the reference subject.
        instruction = (f"Paint the surface of the object in <image_1> with {look}. The object in <image_1> keeps "
                       "its exact shape, silhouette, proportions and camera; it does not become the subject of "
                       + (shown if slots else "the description") +
                       ". Only colors, patterns and surface details are transferred onto its surface, with flat "
                       "shadowless lighting.")
    return json.dumps({
        "short_description": f"An object on a pure white background with {look}. Unlit albedo base color render: "
                             "every surface shows only its own color, with no shading.",
        "objects": [{
            "description": f"The object in <image_1>, with {look}.",
            "location": "center", "relative_size": "large within frame",
            "shape_and_color": f"exact silhouette and shape of the object in <image_1>",
            "appearance_details": details or look,
            "relationship": "single isolated object", "orientation": "identical to <image_1>",
        }],
        "background_setting": "pure white seamless background",
        "lighting": {"conditions": "perfectly flat, uniform ambient illumination for an unlit albedo texture",
                     "direction": "none; equal light from every direction",
                     "shadows": "no shadows, no shading, no ambient occlusion, no highlights or reflections"},
        "aesthetics": {"composition": "centered, identical framing and pixel positions to <image_1>"},
        "photographic_characteristics": {"depth_of_field": "deep, everything in focus", "focus": "sharp",
                                         "camera_angle": view_description(command),
                                         "lens_focal_length": "orthographic, no perspective"},
        "style_medium": "3D render", "artistic_style": "unlit albedo texture",
        "context": "An unlit base-color (albedo) pass of a 3D model, used for texture projection.",
        "text_render": [],
        "edit_instruction": instruction,
    }, ensure_ascii=False)


def preserve_trusted_details(image, command):
    """Keep one sharp detail source in trusted cores; feather only a narrow boundary.

    The paint guide drives generation; this final lock protects strong existing albedo from the model
    re-rendering it slightly differently.
    """
    if command.get("conditioning") != "confidence" or not command.get("protectDetails", True):
        return image, None
    import numpy as np
    from PIL import Image, ImageFilter
    with Image.open(command["accepted"]) as source:
        accepted = source.convert("RGB")
    with Image.open(command["feedbackMask"]) as source:
        confidence = source.convert("L")
    if accepted.size != image.size or confidence.size != image.size:
        raise ValueError("Previous-detail guide dimensions do not match the generated image")
    weights = np.asarray(confidence)
    if command.get("continuityVersion", 1) >= 2:
        # Keep the continuous confidence ramp rather than introducing a new hard 224/255
        # cut through the trusted patterns when restoring their exact source pixels.
        alpha = np.clip((weights.astype(np.float32) / 255 - .5) * 2, 0, 1)
        alpha = alpha * alpha * (3 - 2 * alpha)
    else:
        # Legacy candidates: avoid broadly mixing two independently generated versions of fine details.
        core = Image.fromarray(np.where(weights >= 224, 255, 0).astype(np.uint8))
        alpha = np.asarray(core.filter(ImageFilter.GaussianBlur(1.25)), dtype=np.float32) / 255
    alpha[weights == 0] = 0  # Empty, hidden and unreliable areas cannot inherit paint.
    if not np.any(alpha):
        return image, None
    def decode(rgb):
        rgb = np.asarray(rgb, dtype=np.float32) / 255
        return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    linear = decode(image.convert("RGB")) * (1 - alpha[..., None]) + decode(accepted) * alpha[..., None]
    srgb = np.where(linear <= .0031308, linear * 12.92, 1.055 * np.maximum(linear, 0) ** (1 / 2.4) - .055)
    output = Image.fromarray(np.rint(np.clip(srgb, 0, 1) * 255).astype(np.uint8))
    mask = Image.fromarray(np.rint(alpha * 255).astype(np.uint8))
    return output, mask


def background_guard(image, command):
    """Mask of mesh pixels left as background white where the guide showed bare clay (0 = do not project).

    Later views must not project an unpainted region as white paint. Returns None when nothing is guarded.
    """
    if command.get("conditioning") != "confidence":
        return None
    import numpy as np
    from PIL import Image
    with Image.open(command["silhouette"]) as s, Image.open(command["feedbackMask"]) as m:
        mesh = np.asarray(s.convert("L")) > 127
        clay_area = np.asarray(m.convert("L")) == 0
    white = np.all(np.asarray(image.convert("RGB")) >= 245, axis=2)
    guarded = mesh & clay_area & white
    if not guarded.any():
        return None
    return Image.fromarray(np.where(guarded, 0, 255).astype(np.uint8))


def save_generation_images(image, command, output):
    """Keep the provider image before legacy detail restoration for independent layers."""
    source_path = os.path.splitext(output)[0] + "-source.png"
    image.save(source_path, format="PNG")
    image, kept = preserve_trusted_details(image, command)
    preservation_path = guard_path = ""
    if kept is not None:
        preservation_path = os.path.splitext(output)[0] + "-retained.png"
        kept.save(preservation_path, format="PNG")
    guard = background_guard(image, command)
    if guard is not None:
        guard_path = os.path.splitext(output)[0] + "-guard.png"
        guard.save(guard_path, format="PNG")
    image.save(output, format="PNG")
    return source_path, preservation_path, guard_path


def geometry_alignment(image, silhouette_path):
    """Conservative white-background silhouette diagnostic, not a geometry solver.

    White/transparent-looking materials are ambiguous and are explicitly unscored.
    The caller keeps the unmodified output for inspection rather than hiding drift
    by clipping it to the expected mask.
    """
    if not silhouette_path:
        return None
    import numpy as np
    from PIL import Image
    with Image.open(silhouette_path) as source:
        expected = np.asarray(source.convert("L")) > 127
    rgb = np.asarray(image.convert("RGB"), dtype=np.int16)
    actual = np.max(255 - rgb, axis=2) > 32
    if actual.shape != expected.shape:
        raise ValueError("Geometry mask dimensions do not match the generation")
    if actual.sum() < expected.sum() * .25:
        return None
    union = np.count_nonzero(actual | expected)
    return float(np.count_nonzero(actual & expected) / max(1, union))


def emit(event, **data):
    with output_lock:
        print(json.dumps({"protocol": PROTOCOL, "event": event, **data}), flush=True)


def reader():
    for line in sys.stdin:
        try:
            command = json.loads(line)
            if command.get("protocol") != PROTOCOL:
                raise ValueError("Unsupported worker protocol")
            if command.get("command") == "cancel":
                cancelled.set()
            else:
                commands.put(command)
        except Exception as exc:
            emit("error", id="", message=str(exc))
    cancelled.set()
    commands.put({"command": "shutdown"})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="")
    parser.add_argument("--profile", choices=["standard", "memory"], default="standard")
    parser.add_argument("--fake", action="store_true")
    parser.add_argument("--trace", action="store_true", help="Maintainer diagnostic stack dump")
    args = parser.parse_args()
    if args.trace:
        import faulthandler
        faulthandler.dump_traceback_later(30, repeat=False)
    # NumPy/OpenBLAS DLL initialization can deadlock on Windows if a second Python
    # thread is already blocked reading a pipe. Load native libraries before the
    # cancellation reader starts; the fake worker remains standard-library-only.
    if not args.fake:
        emit("progress", fraction=0, message="Loading local AI libraries")
        import numpy  # noqa: F401
        import torch  # noqa: F401
        from diffusers import BriaFibo2Pipeline  # noqa: F401
    threading.Thread(target=reader, daemon=True).start()
    pipe = None
    fonts = []
    described = {}  # Reference appearance descriptions, by file identity, for the life of the worker.
    emit("ready", fake=args.fake, capabilities=["generate", "confidence-feedback", "view-aware",
                                                 "background-guard", "cancel", "health", "shutdown"])

    def prepare():
        nonlocal pipe, fonts
        if args.fake or pipe is not None:
            return
        emit("progress", fraction=0, message="Loading local AI libraries")
        import torch
        from diffusers import BriaFibo2Pipeline
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is unavailable. Check the NVIDIA driver and AI runtime.")
        eligible = [index for index in range(torch.cuda.device_count())
                    if torch.cuda.get_device_capability(index)[0] >= 8
                    and torch.cuda.get_device_properties(index).total_memory >= 12000 * 1024 * 1024]
        if not eligible:
            raise RuntimeError("The AI engine requires an Ampere-or-newer NVIDIA GPU with at least 12 GB VRAM.")
        index = max(eligible, key=lambda i: torch.cuda.get_device_properties(i).total_memory)
        torch.cuda.set_device(index)
        device = torch.device("cuda", index)
        emit("progress", fraction=0, message="Loading local model weights")
        pipe = BriaFibo2Pipeline.from_pretrained(args.model, processor=image_only_processor(args.model),
                                                 torch_dtype=torch.bfloat16, local_files_only=True)
        linear_patch_embed(pipe.text_encoder.model.visual.patch_embed)
        # FP8 halves the transformer (8 GB) and uses Ada/Hopper tensor cores; Ampere keeps bfloat16.
        fp8 = torch.cuda.get_device_capability(index) >= (8, 9)
        if fp8:
            emit("progress", fraction=0, message="Preparing FP8 transformer")
            quantize_fp8(pipe.transformer, device)
        emit("progress", fraction=0, message="Preparing GPU memory profile")
        if args.profile == "standard" and fp8:
            pipe.to(device)  # Resident: about 16 GB of weights, 19 GB peak at 1024.
        elif fp8 or args.profile == "standard":
            pipe.enable_model_cpu_offload(gpu_id=index)
        else:
            pipe.enable_sequential_cpu_offload(gpu_id=index)
        pipe.set_progress_bar_config(disable=True)
        fonts = [os.path.join(args.model, "fonts", name) for name in ("Aileron-Regular.otf", "DejaVuSans.ttf")]
        fonts = [path for path in fonts if os.path.isfile(path)]
        if cancelled.is_set():
            raise InterruptedError("Cancelled")

    def run(images, prompt, size, seed, steps=STEPS, progress=None):
        import torch
        # A full allocator pool makes Windows silently spill VRAM to system memory (several times slower).
        torch.cuda.empty_cache()

        def step_callback(pipeline, step, timestep, callback_kwargs):
            if cancelled.is_set():
                raise InterruptedError("Cancelled")
            if progress:
                progress(step + 1)
            return callback_kwargs
        with torch.inference_mode():
            return pipe(prompt=prompt, image=images, height=size, width=size, num_inference_steps=steps,
                        guidance_scale=1.0, fonts=fonts, generator=torch.Generator("cpu").manual_seed(seed),
                        callback_on_step_end=step_callback).images[0]

    while True:
        command = commands.get()
        action = command.get("command")
        request_id = command.get("id", "")
        if action == "shutdown":
            break
        cancelled.clear()
        try:
            if action == "health":
                prepare()
                if not args.fake:
                    from PIL import Image
                    emit("progress", fraction=0, message="Running a small test generation")
                    # Exercise the actual generation kernels before activating an installation.
                    run([Image.new("RGB", (512, 512), (128, 128, 128))],
                        json.dumps({"short_description": "A plain gray square.",
                                    "edit_instruction": "Keep the gray square unchanged."}), 512, 0, steps=1)
                emit("health", id=request_id, ok=True)
            elif action == "generate":
                started = time.monotonic()
                timings = {}
                prepare()
                timings["prepare"] = time.monotonic() - started
                output = command["output"]
                source_path = output
                preservation_path = guard_path = ""
                descriptions = []
                alignment = None
                peak = 0
                if args.fake:
                    shutil.copyfile(command.get("neutral") or command["normal"], output)
                else:
                    import torch
                    size = int(command.get("size", 1024))
                    if size not in (512, 1024):
                        raise ValueError("Unsupported generation size")
                    torch.cuda.reset_peak_memory_stats()
                    references = load_references(command)
                    descriptions = []
                    for path, reference in zip(command.get("references", []), references):
                        key = (os.path.abspath(path), os.path.getmtime(path), os.path.getsize(path))
                        if key not in described:
                            emit("progress", id=request_id, fraction=0, message="Reading reference image")
                            described[key] = describe_reference(pipe, reference)
                        descriptions.append(described[key])
                    images = [guide_image(command)] + references
                    images[0].save(os.path.splitext(output)[0] + "-guide.png", format="PNG")
                    prompt = generation_caption(command, descriptions)
                    emit("progress", id=request_id, fraction=0, message="Preparing appearance prompt")
                    generated_at = time.monotonic()
                    image = run(images, prompt, size, int(command.get("seed", 0)),
                                progress=lambda step: emit("progress", id=request_id, fraction=.95 * step / STEPS,
                                                           message=f"Generating view: step {step}/{STEPS}"))
                    timings["generation"] = time.monotonic() - generated_at
                    if image.size != (size, size):
                        raise RuntimeError("The AI returned an image with an unexpected size")
                    # Check geometry on the generation before trusted paint is restored.
                    alignment = geometry_alignment(image, command.get("silhouette"))
                    if command.get("conditioning") == "confidence" and command.get("protectDetails", True):
                        emit("progress", id=request_id, fraction=.97, message="Keeping reliable previous details")
                    source_path, preservation_path, guard_path = save_generation_images(image, command, output)
                    peak = int(torch.cuda.max_memory_allocated() / (1024 * 1024))
                if cancelled.is_set():
                    raise InterruptedError("Cancelled")
                emit("result", id=request_id, output=output, seconds=time.monotonic() - started,
                     sourceImage=source_path,
                     timings=timings, alignment=alignment, preservationMask=preservation_path,
                     referenceDescriptions=descriptions,
                     guardMask=guard_path, peakVramMiB=peak)
            else:
                raise ValueError("Unknown worker command")
        except Exception as exc:
            traceback.print_exc(file=sys.stderr)
            emit("error", id=request_id, cancelled=isinstance(exc, InterruptedError), message=str(exc))
    del pipe
    gc.collect()


if __name__ == "__main__":
    main()
