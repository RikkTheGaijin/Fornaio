# Fornaio

A portable Windows application for baking ambient occlusion through hair-card opacity masks and transferring textures between UV layouts. Uses NVIDIA OptiX hardware ray tracing, with a native C++/Qt desktop interface.

## Run

Open `Fornaio.exe` inside the portable folder. Keep the DLLs, plugin folders, and PTX files beside it. No installer, system Python, Blender, CUDA SDK, or Substance Painter is needed to run it. Processing is local. The optional AI Texturing setup downloads its engine and model; AO baking and texture transfer work without installing AI components.

Requires Windows 10/11 x64, an NVIDIA RTX GPU, and an R590-or-newer NVIDIA driver (OptiX 9.1). Tested on the RTX 4090 with driver 617.14. See [NVIDIA's driver requirement](https://developer.nvidia.com/designworks/optix/download).

1. Load your FBX or OBJ and a PNG, TIFF, or TGA opacity mask.
   Optionally click **Load head / occluder…** to import a second FBX/OBJ. It casts opaque occlusion onto the hair and needs no UVs. **Remove** returns to hair-only baking.
2. Select **Mask UV**, the UV set used to look up the hair texture.
3. Select **AO UV**, preferably a unique, non-overlapping 0–1 UV atlas. These can be different sets. FBX supports multiple UV sets; OBJ only provides one.
4. Choose the mask channel. Alpha is selected automatically when the image contains an alpha channel; otherwise red. For an opaque RGBA image containing grayscale hair in RGB, select **Red** explicitly.
5. Start with **Draft**, then use **Quality** for the final bake. Click **Bake AO**.
6. Inspect **Baked AO** in 3D or the **AO texture** tab, then **Export PNG**. Export stays in the fixed bottom bar, is also available in the texture tab, and has a **Ctrl+S** shortcut.

Try `samples/hair_400_cards.fbx` with `samples/strands.png`, Mask UV1 and AO UV2. `samples/stacked_cards.fbx` is a smaller transmission demonstration.
For a complete hair/head example, also load `samples/head_example_no_uv.obj` as the occluder. Export your own hair and head from the same DCC scene using matching coordinates, axes, and units (preferably the same file format/export settings). Imported transforms are preserved; the app does not automatically align or resize the meshes. The head appears tan in the preview so you can check its position.

**The result must be sampled using the selected AO UV set in your final material.** If your downstream material only samples UV1, you must supply a unique atlas in UV1, or configure that material to sample the AO texture from UV2. This app does not rewrite mesh UVs.

## Texture Transfer tab

The second tab reprojects texture sets from one UV layout to another. Use it for two exports of the same geometry that differ only in UVs.

1. **Meshes:** load the source and target FBX/OBJ files. If both layouts are in one FBX, use **Same file** and pick a different UV set for each side.
   - **Auto** matches identical faces exactly. It falls back to ray projection along the target normal, limited by the front/back distances, for faces with no match.
   - **Projection** always uses rays.
2. **Source textures:** add PNG/TIFF/TGA files or a folder.
   - Files group into sets by prefix and suffix: `body_ALB.png` and `body_NRM.png` form set `body`.
   - The material table maps each source material to a set, with an automatic guess.
3. **Object pairing:** restricts each target object to its same-named source object, so neighbouring meshes never receive each other's texels. **Auto-pair** fills the table, and you can edit it. `(any)` allows all source objects; `(none)` leaves the object empty.
4. **Map types:** the suffix table controls how each map is filtered and what fill value is used when a set lacks that map. A missing map produces a warning.
   - Colour maps are filtered in linear light.
   - Data maps are filtered as-is.
   - Normal maps are re-expressed in the target's MikkTSpace tangent frame. OpenGL (Y+) or DirectX (Y−) can be chosen per side.
5. **Output:** one texture set per target material, with an editable prefix and resolution. Auto resolution uses the largest contributing source.
   - Several source sets can feed one output. **Single texture set** writes all target materials into one set (one shared prefix and resolution).
   - Bit depth and alpha follow the sources.
   - Settings: supersampling, filter (auto = bicubic, with EWA where the source is minified), island-aware sampling, padding, and overlap averaging.

Preview with the synchronized 3D split view (source | target) or the 2D before/after view with UV wireframes. **Save job** writes a JSON file that the CLI can run. Existing outputs are only replaced after confirmation. The GPU (OptiX) is used by default, and the CPU reference implementation is offered if the GPU fails. Only one GPU job runs at a time across both tabs.

Source taps are clamped to the UV tile of each sample: a 0–1 texture never bleeds across its opposite border, while UVs outside 0–1 still repeat.

## AI Texturing tab

The third tab generates base-color images from mesh views and projects them into the selected UV layout. This is an experimental image-editing workflow: normal/depth guides encourage geometry alignment, but results need inspection and may contain lighting or inconsistent details.

1. Choose the **Image model**. **GPT Image 2 · cloud** is the default and needs no download: Fornaio uses the Codex CLI signed in with your ChatGPT account (found on the PATH or inside the VS Code ChatGPT extension; `FORNAIO_CODEX` selects another executable), or the OpenAI Images API when `OPENAI_API_KEY` is set. The camera guide, the first reference image and the prompt are uploaded to OpenAI; your plan's image limits or API billing apply. A view takes about a minute. **Fibo 2.0 Edit Turbo · local** is optional: click **Download & Enable AI Texturing** to download the private runtime and Fibo (about 28 GB), verify SHA-256 hashes and check the model. Fibo is gated on Hugging Face: accept its license at huggingface.co/briaai/Fibo-2.0-edit-turbo, then enter an access token (or set `HF_TOKEN` / log in with the Hugging Face CLI). Fibo is CC BY-NC (non-commercial).
2. Downloads default to `%LOCALAPPDATA%\Fornaio\AI`. **Change storage location…** selects another directory; an installed engine is copied and checked there without downloading it again. Files at the previous location are preserved, and a failed move keeps using the original engine. Cancelled downloads resume when setup is started again, including after an application restart. **Repair** rechecks downloads. **Remove** removes only AI components at the selected location.
3. Load an FBX/OBJ, select a UV set, describe the appearance and/or add reference images. Every material needs unique, non-overlapping UVs inside 0–1; different materials may use the same UV coordinates. Static transforms are preserved.
4. Choose **Single**, orbit the square viewport, and click **Generate image**. The left **Generate** panel places the image preview above the prompt and reference thumbnails, beside the 3D viewport. The **Generated view / mask** tab provides a larger canvas. Both canvases share the same mask: left-drag excludes areas; right-drag restores them. Inspect alignment before clicking **Project camera view** in the fixed bottom bar. Projection always uses the captured camera, even if the preview camera has moved. **Unlit base color** is the default 3D display. **Untextured areas** marks missing coverage with red/dark diagonal hatching; this preview overlay is never generated, projected, or exported.
5. Choose **Multiview** and click **Generate** to texture the mesh from the listed cameras (front, the 45-degree ring, back and top). With GPT Image 2 each view sees the paint that earlier views projected well (locked) and gray clay elsewhere; it paints only the clay, its version of the locked pixels is replaced by the accepted paint, and only new areas are projected. Back faces are never rendered into guides, so inner surfaces such as a collar seen from behind are not painted with outer details. **Keep previous details** defaults to 80%: the next view receives a confidence-filtered rendering of accepted paint, normal/depth/silhouette guides, a reuse mask, and your prompt/references. Strong previous projections supply sharp details. Neighboring paint stays visible as context while its reuse mask permits repair; very poor paint and empty areas remain neutral. Reliable paint at the incoming projection's falloff receives extra protection. The first visible accepted view for a selected material supplies a stable full-object appearance reference, helping retain grain, crack and chip scale between cameras. Set this control to **0%** for fully independent generation. Camera projection mode can repaint covered surfaces; quality blending and fill-only remain optional projection choices. Inspect **Coverage** or the hatch overlay and add manual views for holes and cavities.
6. The right **Projection layers** panel lists accepted views, newest first, with thumbnails and eye toggles. Hide/show projections to isolate a seam; the 3D preview, UV texture, coverage, future generation feedback, and exports use the visible stack. Visibility changes support undo/redo and never rerun AI generation. Recent compositions and GPU textures are cached for fast repeat comparisons; an uncached combination still composes in the background. Older projects reconstruct UV layers once from their saved cameras/images on the first toggle; this can take longer. Save/open a `.fornaioai` project to preserve layer data/visibility, accepted atlases, candidates, masks, settings, and camera positions. Project files embed generated images and compressed UV paint data and can be large. Mesh files must remain available and unchanged; appearance reference paths are retained.
7. Export one sRGB `_ALB.png` per material. Edit each output name in **UV texture**. Choose 1024, 2048 (default), or 4096 atlas resolution. Generation defaults to 1024; **Generation size → 512 · Fast draft** makes smaller camera images for quicker iteration without resizing atlases. The size is saved in projects; older projects default to 1024. Exported textures must use the selected mesh UV set.

**Package size:** the main application and source bundles contain no model weights, Python distribution, PyTorch, or large AI dependencies. The pinned optional Fibo download is approximately 28.4 GB; setup displays exact totals and free-space requirements. An existing cached installation downloads only the additional components. Downloads use verified, prebuilt Python/wheel archives and model files rather than resolving or compiling packages on the user's computer. Once installed, Fibo generation is offline and its descriptions, mesh renders, and references are not uploaded.

The local Fibo engine requires an Ampere-or-newer RTX GPU (RTX 30 series or newer), at least 12 GB VRAM, and driver R590 or newer. **Standard** targets 24 GB; **Memory saving** uses sequential CPU offloading and tiled VAE processing for 12–16 GB cards, with slower inference and additional system-memory use. Actual smaller-card validation is recorded separately in `VALIDATION.md`.

Only one GPU job runs at a time across all three tabs. Downloading does not reserve the GPU; the final setup check waits for existing GPU work. The AI worker and loaded weights stay warm between generations and projections. The standard profile retains frozen CPU weight storage to avoid copying unchanged GPU weights back after each view. The worker caches the last prompt's text embeddings and prefers fast supported attention kernels, with fallbacks. Geometry, references, and accepted paint are still processed for each new request. Starting AO or Texture Transfer, changing the memory profile/storage, repairing the engine, cancelling a job, or closing Fornaio releases the worker. Cancellation and inference failure preserve accepted projections.

Expand **Mesh, materials and generation settings** for UVs, atlas resolution, memory profile, materials, mask brush, albedo recovery, and comparison with the original lighting. **New variation each generation** is on by default; disable it to use a fixed seed. The actual seed is recorded with each candidate/projection. Generation mode, seed preference, and hatch visibility are saved in projects. **Show captured geometry outline** optionally adds a cyan silhouette for inspection. A conservative white-background silhouette check on the raw generation pauses automatic projection when overlap drops below 75%. White materials can be ambiguous, and passing this check does not guarantee internal features align.

**AI camera and sharpness:** **Orthographic** is on by default: views have no perspective, so every pixel sees the surface along the same direction. When you click **Generate image**, the captured camera keeps your orbit direction but re-frames the view to the mesh's view-space bounding box, filling 95% of the square image. If you have zoomed in, it frames only the part of the bounds inside the current view, so close-ups stay close. **Frame mesh** applies the same fit to the viewport; automatic views fit each direction too. Turn Orthographic off to use a perspective lens (FOV defaults to 20 degrees; changing it moves the camera to preserve framing). **Maximum projection angle** defaults to 60 degrees in Projection blending, with a smooth ten-degree falloff, and is adjustable from 35 to 80 degrees. **Paint every visible face** is on by default: untextured surfaces beyond that angle still receive the current view down to about 89 degrees, at low confidence, so a later camera-facing view replaces the stretched paint. Already textured surfaces keep the angle limit. Old accepted layers keep their original cameras and paint; regenerate views to benefit from these settings.

**Keep previous details** changes how much prior paint qualifies for reuse, rather than blending every old detail at that percentage. Quality comes from the previous projection's camera angle, feather, and painted mask. More reuse includes more reliable surface; 0% excludes previous paint entirely in every projection mode. The first view is independent because no paint exists yet. **Protect trusted details** is enabled in advanced settings: it also anchors reliable paint during Klein denoising, restores strong accepted base color after generation, and protects the original atlas texels during projection. This avoids duplicated details and repeated camera/UV resampling in trusted cores. Their original confidence is retained for later views. Disable the lock to allow reinterpretation, or use 0% continuity to change the overall appearance. Continuity settings, guide snapshots, retained masks, and source confidence survive saving; changing the control affects the next generation. These changes use the existing Klein installation.

**Projection blending** defaults to **Keep existing paint (paint around it)**, including automatic mode: paint that an earlier view saw well (projection confidence at least 0.1) is locked, and a new view paints only uncovered or grazing-angle areas, so independently generated views never blend different versions of a detail into ghosts. Projects saved earlier keep their saved mode. **Camera projection (repaint)** remains available: Each accepted image paints over existing texture from its captured camera, even on a fully covered mesh. Camera-facing areas receive the new image strongly; oblique surfaces and camera, silhouette, occlusion, and painted-mask edges blend with a soft falloff. Previous projection confidence does not block repainting. **Strength** controls influence (values over 100% extend it onto oblique surfaces); **Feather width** controls edge softness, and **Angle falloff** favors camera-facing surfaces. Defaults are 100% strength, 6% feather, angle falloff 2, and a 60-degree maximum angle. Strength cannot override that angle limit.

Use **Blend by camera quality** to favor stronger existing projections, or **Fill uncovered only** to preserve all accepted texels. **Match colors in overlap** is optional and off by default so deliberate color changes remain visible. Generation continuity is separate from these projection controls. Excluded/occluded areas and unselected materials remain protected. Blending uses linear light and tracks coverage independently from camera confidence and padding. Undo/redo and saved projects retain confidence and settings. Projects using the former 1.2.3 default migrate to camera repainting for future projections; their saved colors remain unchanged. No additional AI downloads are required. Generated details, tone, and geometry can still differ in weak or new areas; masked denoising constrains trusted detail but does not guarantee continuity in every weak or newly generated area. Inspect the candidate and mask incorrect areas.

The first release handles opaque meshes and base color. PBR generation, UDIMs, unwrapping, transparency-aware texturing, custom training, and mesh export are not included. Generated color is not guaranteed physically correct albedo. Selecting a different mesh, UV set, or atlas resolution starts a new atlas; save a project first if you need the existing result.

### Maintaining the optional AI components

`ai/manifest.json` pins public download URLs, exact sizes, SHA-256 hashes, model revision, and resolved dependency versions. It deliberately excludes duplicate checkpoint formats and training assets. `scripts/resolve_ai_manifest.py` refreshes this metadata at release preparation time; users never run it (Fibo is gated, so set `HF_TOKEN` or log in with the Hugging Face CLI first). Review component licenses and rerun real-engine checks before publishing a new manifest.

Tests: `fornaio_ai_tests` covers capture, projection, ownership, alignment and the panel (set `FORNAIO_REAL_GPT=1` to include one real GPT Image 2 generation). `tests/ai_worker_tests.py` and `tests/ai_inference_tests.py` test the local Fibo worker with any Python that has NumPy, Pillow and PyTorch (`python -m unittest tests/ai_worker_tests.py`). `fornaio_ai_eval` drives capture, `ownproject` (the panel's GPT projection path) and rendering from scripts for end-to-end evaluation without the UI.

For offline distribution, run `python scripts/ai_offline_pack.py --cache <AI-storage-directory> --out <separate-offline-pack-directory>` on a completed installation. This verifies and copies the cached components beside a manifest. Keep this large optional pack separate from Fornaio's normal download. Users select its `manifest.json` with **Import offline pack…**; it must match the bundled release catalogue.

## UV2 layout

**Generate UV2…** (AO Baker tab, section 02) takes the loaded FBX, keeps UV1 untouched and writes a copy with a new UV2: every UV1 island packed into the 0–1 tile at one shared texel density, without overlaps. Mirrored and stacked UV1 islands each get their own space, so the result bakes with unique AO. The copy is loaded with AO baking set to UV2; the **UV layout** preview tab shows it. An existing second UV set is replaced.

- **UV set name** (default `UV2`) names the generated set: letters, digits and underscores, different from the UV1 name. Leave it empty to keep the name of a replaced second UV set.
- **Island spacing** is the minimum gap between islands in pixels at the selected bake resolution. Keep it at least twice the mip levels you need to protect.
- **Allow 90° island rotation** is off by default. When on, the packer also tries turning islands and keeps rotation only when it fits more texel area into the tile.
- The packer runs a parallel search over several skyline strategies (best-fit, height/width/area orders) and keeps the densest layout. Tall hair-card strips are stacked in columns, similar to RizomUV packing.
- Only binary FBX 7.x is supported; the output keeps every other FBX node byte-for-byte. ASCII FBX and OBJ are rejected.

## Controls and interpretation

- Left drag orbits; right/middle drag pans; wheel zooms; **Frame model** resets the camera. Red geometry marks invalid output UV faces. The image viewer supports wheel zoom, drag pan, fit, and 1:1 inspection.
- Black mask pixels transmit light; white blocks it; gray partially transmits. At successive intersections, ray transmission multiplies by `1 − opacity`. There is no fixed transparency-layer limit. Masks are sampled bilinearly as linear data, without sRGB decoding.
- The optional head/occluder is always fully opaque and two-sided, regardless of its materials, UVs, or the hair mask. It casts AO only; it never occupies pixels in the output atlas. Maximum ray distance applies to the head too. Loading, replacing, or removing it preserves the previous completed bake until a new one succeeds.
- AO is **front-side**, relative to imported normals. Occluders are two-sided. Fix reversed normals in your DCC if the intended front side is wrong.
- **Max distance** and **Ray bias** use imported model units. FBX units may differ from those displayed in your DCC; the app shows the imported bounding-box diagonal. Zero distance means unlimited. Automatic bias is one millionth of that diagonal; reduce it for exceptionally close sheets.
- Quality defaults: 2048², 256 rays per subpixel sample, 2×2 subpixel sampling, 8-pixel padding, and 16-bit grayscale PNG. Draft: 512², 32 rays, 1×1 sampling. Output sizes range from 256² to 8192².
- Transparent subpixel samples are excluded from AO normalization. Opacity is not multiplied into the exported AO. Empty pixels start white; padding propagates the nearest front's value without averaging across UV islands. Very close islands still need enough atlas gutter for your downstream mipmaps.
- **Average AO where UVs overlap** is enabled by default. Overlaps produce an amber warning and do not block baking. All faces covering a subpixel contribute opacity-weighted AO; shared tessellation edges are counted once, including mirrored islands. This preserves your geometry and UVs, but the shared output texel cannot retain a different AO for every face. Disable this option to require unique UVs. Missing UVs, invalid geometry, and out-of-tile output UVs still block baking.
- PNG values are linear: 0 = occluded, 1 = unoccluded. Export embeds a linear `gAMA` chunk and preserves all 16 bits. Configure the image as non-color/data in your renderer. A general image viewer may display it differently from the raw grayscale preview.
- The 3D viewport uses ordered opacity dithering for responsive previews. Baking uses graded transmission. The preview is 8-bit; export retains the full bake precision. No denoiser blurs the strands.
- Scrolling over a dropdown or number field scrolls the settings panel without changing its value. Use clicks, typing, or keyboard navigation to edit values.
- Changing settings or inputs preserves the last completed bake and marks it as needing a rebake. **Baked AO** continues to use that bake's geometry, mask, and UV settings; other preview modes show the current inputs. Export always saves the last completed bake, using the chosen PNG bit depth. Only a successful new bake replaces it.
- Bake preferences persist per Windows user. Cancellation discards only the incomplete new result; the previous completed bake remains exportable. Export is an atomic file replacement, so a write failure does not destroy an existing PNG.

## Boundaries

One hair geometry file (which may contain multiple meshes), one optional opaque occluder file, one shared hair mask, one output atlas, front-side AO. UV selections refer to layer ordinals across the hair meshes; the chosen sets must exist on every hair mesh. Occluder UVs are ignored. Static imported transforms and normals are preserved; animation, skin deformation, material shader networks, automatic unwrapping, UDIMs, and general mesh export are not included (the UV2 packer only rewrites UV layers of binary FBX files). The CPU implementation is a small validation reference, not a production fallback.

Mask-aware AO is a sampled estimate. Raise the ray count to reduce noise and supersampling to resolve subpixel strands. Features substantially smaller than an output texel need a higher resolution. Test coverage and practical limits are documented in `VALIDATION.md`.

## Build from source

Build prerequisites: Visual Studio 2022 C++ tools, a Windows SDK, CUDA Toolkit 12.8, and Qt 6.8.3 MSVC2022 x64 (`qtbase`, `qtimageformats`). The source bundle includes pinned ufbx, stb, and OptiX headers.

When cloning, include the pinned dependencies with `git clone --recursive https://github.com/RikkTheGaijin/Fornaio.git` (or run `git submodule update --init` in an existing clone).

```powershell
python -m venv .build-tools
.\.build-tools\Scripts\python -m pip install aqtinstall==3.3.0
.\.build-tools\Scripts\python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .deps/Qt --modules qtimageformats --archives qtbase qtimageformats
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

The build script discovers Visual Studio and its CMake, builds the CUDA program to PTX, builds Release binaries, and deploys Qt DLLs beside the executables. Set `CUDA_PATH` if CUDA is not found automatically. The GUI loads `bake.ptx` from its own directory, independent of the working directory.

```powershell
.\build\Release\fornaio_cli.exe --self-test --artifacts artifacts/tests
.\build\Release\fornaio_ui_tests.exe
.\build\Release\fornaio_ai_tests.exe
python scripts/integration_tests.py
powershell -ExecutionPolicy Bypass -File scripts/package.ps1
```

The integration script uses only Python's standard library. Regenerating FBX fixtures optionally uses Blender via `scripts/generate_fixtures.py`; fixtures are included, so neither building nor testing requires Blender.

## CLI

```powershell
.\fornaio_cli.exe --mesh samples/stacked_cards.fbx --mask samples/strands.png --channel red --mask-uv 1 --ao-uv 2 --size 2048 --rays 256 --ss 2 --bits 16 --out hair_AO.png
.\fornaio_cli.exe --inspect samples/stacked_cards.fbx
.\fornaio_cli.exe --mesh samples/single_card.obj --occluder samples/occluder_no_uv.obj --mask samples/half.png --size 512 --out hair_with_occluder.png
.\fornaio_cli.exe --help
.\fornaio_cli.exe --generate-uv2 hair.fbx --uv2-out hair_UV2.fbx --size 2048 --uv-spacing 4 [--uv-rotate] [--uv2-name hairAO]
```

```powershell
.\fornaio_cli.exe --transfer job.json              # GPU
.\fornaio_cli.exe --transfer job.json --reference  # CPU reference implementation
.\fornaio_cli.exe --transfer job.json --overwrite  # replace existing outputs
```

A transfer job is the JSON saved by the tab. Relative paths resolve against the job file. UV indices in the job are zero-based. Only `source`, `target`, `output_dir` and `sets` are required. When omitted, `suffixes`, `materials`, `pairing.pairs` and `outputs` are auto-filled, and `settings` use the defaults:

```json
{"source": {"mesh": "src.fbx", "uv": 0}, "target": {"mesh": "dst.fbx", "uv": 1}, "output_dir": "out",
 "sets": [{"name": "body", "files": {"ALB": "body_ALB.png", "NRM": "body_NRM.png"}}],
 "settings": {"mode": "auto", "supersampling": 3, "filter": "auto", "padding": 16, "source_normal_directx": false}}
```

The transfer prints a JSON report with per-map outputs, exact/projected triangle counts and warnings.

For the AO commands, UV numbers are one-based. The CLI emits progress on stderr and a JSON result on stdout. `--reference` enables the slow CPU checker for tiny fixtures. CLI resolutions below 256 are allowed for automated tests. `--benchmark` uses an additional synthetic stack stress fixture; the recorded hair benchmarks use the supplied curved 400-card FBX instead.

Mirrored UV islands are supported. Overlap warnings identify the first offending mesh and zero-based imported polygon numbers. Diagnostics stop after 64 overlapping triangle pairs, reporting a lower bound instead of exhaustively enumerating dense shared UV layouts; hard-error validation still checks every face. A folded or self-crossing polygon can overlap its neighbours even when separate islands are well spaced. The CLI averages overlaps by default; `--strict-uv` restores rejection. For a detailed JSON report with UV coordinates and triangle pairs, use `fornaio_cli --inspect mesh.fbx --ao-uv 2`.

## Implementation

`core` handles scene import, mask decoding, exact-area UV overlap checks, UV-island connectivity, spatial binning, PNG export, and CPU reference tracing. `gpu` owns OptiX/CUDA resources through RAII; the device kernel rasterizes AO UV subpixels and casts cosine-weighted rays. GPU work is dispatched in bounded pixel/ray batches with cancellation checks between launches. `window` and `viewport` provide the asynchronous UI and OpenGL preview. `transfer`, `transfer_gpu` and `transfer_kernel.cu` implement texture reprojection on the CPU and on OptiX. They share their sampling code through `transfer_shared.h`. `transfer_panel` and `transfer_viewport` provide the Texture Transfer tab. `fbx_binary`, `uv_pack` and `uv2` implement lossless binary FBX rewriting, skyline island packing and UV2 generation. Dependency details and licenses are in `THIRD_PARTY_NOTICES.md` and `licenses/`.

## Release notes

### 1.3.0

- Make **GPT Image 2** the default AI Texturing model, with no download: Fornaio uses the Codex CLI signed in with your ChatGPT account (found on the PATH or in the VS Code ChatGPT extension) or `OPENAI_API_KEY`. Replace the local Klein engine with an optional **Fibo 2.0 Edit Turbo** download (gated on Hugging Face; the installer sends your access token to huggingface.co only). Remove the Marigold albedo extension.
- Multiview with ownership (**Keep existing paint**, the new default): paint an earlier view saw well is locked, the next view sees it and paints only gray clay, and only new areas are projected. A tone match measured on the locked area and a crossfade band at the lock's edge remove seams between views.
- Align every generated view to the mesh before projection (local edge registration against the clay shading), and tell the model to take only appearance from the reference, never its pose. Never render back faces into generation guides.
- Orthographic AI camera by default, framed to the mesh's view-space bounds; **Paint every visible face** fills grazing empty surfaces at low confidence; multiview camera grid with per-layer regeneration.

### 1.2.13

- Fix reference-image generation with an empty prompt: explicitly identify appearance images and instruct Klein to replace the untextured placeholder with their material.
- Use an achromatic geometry guide for reference requests to keep normal-map colors from tinting the generated material. Preserve confidence-based continuity and text-only generation.
- Normalize reference transparency and EXIF orientation before inference. No new models or downloads.

### 1.2.12

- Fill enclosed visible holes between accepted projections with the current candidate, allowing angles up to 80 degrees only into empty texels. Keep the sharper maximum angle on fresh outer edges and existing paint.
- Add **Fill enclosed visible gaps**, enabled by default. Save the setting and each new layer's behavior without changing existing saved paint.
- Keep gap fill at low confidence so a better later view can replace it. Reproject an existing candidate to repair gaps without generating again.

### 1.2.11

- Use a 20-degree AI camera with a FOV control, matching viewport/capture lenses, and tighter bounds-based framing. Dolly when changing the lens to preserve framing.
- Default to a 60-degree maximum projection angle with a smooth ten-degree falloff; leave grazing surfaces uncovered instead of painting stretched edge pixels.
- Preserve more reliable overlap during generation and use the first visible accepted view as a stable full-object appearance reference, avoiding crop-induced changes in grain scale.
- Save lens/angle settings and retain immutable cameras and legacy projection behavior for existing layers. Keep Texture Transfer's linked cameras synchronized without notification loops.

### 1.2.10

- Preserve the actual color and neighboring patterns in generation feedback; anchor reliable overlap edges using the incoming camera's angle and feather.
- Add masked latent denoising to keep accepted detail aligned throughout Klein generation, alongside the existing exact detail lock. No extra models or downloads.
- Cache layer compositions, decoded UV operations, and GPU preview textures; avoid repeated thumbnails, UV scaling, and texture uploads when inspecting layer visibility.
- Existing generated passes need regeneration to use the updated continuity behavior.

### 1.2.9

- Add a right-side **Projection layers** panel with thumbnails, camera labels, and visibility eye toggles, newest first.
- Record lossless UV paint operations and compose visible layers without inference or mesh reprojection; preserve original float blend weights, color corrections, confidence, and padding.
- Save layer visibility/base paint in projects and include visibility in undo/redo. Lazily reconstruct layers for existing projects using their saved images and cameras.

### 1.2.8

- Avoid copying unchanged Klein weights back from the GPU in the standard profile; retain CPU sources and release GPU copies after inference or cancellation.
- Cache the most recent prompt's text embeddings and prefer fast supported attention kernels without new runtime downloads.
- Add **Generation size** with 1024 full detail and 512 fast draft, independent of atlas resolution and saved in projects.
- Report decoding progress and retain worker timings with candidates. Refresh packaged worker scripts even when no native code needs relinking.

### 1.2.7

- Add confidence-weighted feedback with **Keep previous details** (default 80%, 0% for independent generation).
- Use strong previous paint, a reuse mask, and a trusted material crop to guide continuation while releasing weak falloff and empty regions.
- Lock trusted details after intrinsic recovery and retain original atlas texels/confidence during projection, avoiding ghosted details and repeated resampling in reliable cores.
- Save continuity settings, guide snapshots, retained masks, and source confidence with projects. Reuse the existing models and warm worker.

### 1.2.6

- Remove the opacity discontinuity between newly covered areas and previous paint in camera repaint mode, including reduced strength and feathered boundaries.
- Blend the coverage join in camera space across geometric UV seams; keep masks, materials, disconnected surfaces, and occlusion protected.
- Resolve ambiguous visibility at the actual surface sample with a native CPU BVH, so dense subpixel triangles do not leave coverage holes. Use consistent shared-edge rasterization and continuous sampling of the feather field.

### 1.2.5

- Generate fresh repaint images from neutral geometry and user references, excluding painted atlas and generated-anchor feedback in both native requests and the worker.
- Choose a new seed per generation by default, with an optional fixed seed and project restoration.
- Mark uncovered surfaces with a preview-only red/dark hatch based on independent coverage.
- Reorganize the Generate panel around Single/Multiview, image preview, prompt, references, and a teal Generate button; retain the full-size image tab and advanced settings.

### 1.2.4

- Make camera repainting the default so new images project over fully covered areas with falloff.
- Process covered views in automatic camera mode and allow the generation prompt to replace old paint.
- Keep quality blending and fill-only as optional preservation modes; migrate the previous protected default.

### 1.2.3

- Blend overlapping AI projections by camera quality, with feathered camera/mask/occlusion boundaries and a soft overlap band.
- Add projection mode, strength, feather width, angle falloff, and overlap color-matching controls.
- Preserve confidence in projects/history and let automatic views improve weak projections after coverage is complete.

### Unreleased

- Remove the Marigold IID albedo extension; its recovery flattened detail. Existing installations keep working and ignore the leftover files.

### 1.2.2

- Add compact Marigold IID Lighting v1.1 albedo recovery, a 2.6 GB optional upgrade using the existing private runtime.
- Recover albedo before projection and compare it with the original generation in either preview; preserve both in projects.
- Retain offline inference, warm workers, verified downloads, and small application/source packages.

### 1.2.1

- Keep the AI worker warm between requests and release it for other modules.
- Show the prompt, reference thumbnails, and editable generated image beside the 3D viewport.
- Correct subpixel visibility at triangle edges, prevent white-background sampling, and match view boundaries in linear light without changing accepted paint in fill mode.
- Add a geometry outline, conservative silhouette-drift pause, and optional experimental albedo cleanup.

### 1.2.0

- Experimental **AI Texturing** tab with local Klein 4B generation, manual/automatic views, masked UV projection, per-material base-color export, undo/redo, and saved projects.
- One-click optional engine setup with checksums, resume, repair, and offline import. Large AI components are excluded from the application/source bundles.
- Shared GPU coordination across AO, transfer, and AI work.

### 1.1.0

- New **UV2 layout** generation in the AO Baker tab: packs the UV1 islands of a binary FBX into a non-overlapping UV2 with uniform texel density, writes an FBX copy and loads it ready for baking.
- Optional 90° island rotation (off by default), adjustable island spacing and a custom UV set name.
- After the first save, **Generate UV2** rewrites the same file, so settings can be iterated without a save dialog; **Save as…** picks a new file.
- New **UV layout** preview tab showing the AO UV set.
- CLI: `--generate-uv2`, `--uv2-out`, `--uv-spacing`, `--uv-rotate`, `--uv2-name`, `--version`.
- The version number is shown in the window title.

### 1.0.0

- Initial release: opacity-aware hair-card AO baking on OptiX and Texture Transfer tab.
