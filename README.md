# Fornaio

A portable Windows application for baking ambient occlusion through hair-card opacity masks and transferring textures between UV layouts. Uses NVIDIA OptiX hardware ray tracing, with a native C++/Qt desktop interface.

## Run

Open `Fornaio.exe` inside the portable folder. Keep the DLLs, plugin folders, and `bake.ptx` beside it. No installer, Python, Blender, CUDA SDK, or Substance Painter is needed to run it. All processing is local; the application has no network features.

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

One hair geometry file (which may contain multiple meshes), one optional opaque occluder file, one shared hair mask, one output atlas, front-side AO. UV selections refer to layer ordinals across the hair meshes; the chosen sets must exist on every hair mesh. Occluder UVs are ignored. Static imported transforms and normals are preserved; animation, skin deformation, material shader networks, automatic unwrapping, UDIMs, and mesh export are not included. The CPU implementation is a small validation reference, not a production fallback.

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

`core` handles scene import, mask decoding, exact-area UV overlap checks, UV-island connectivity, spatial binning, PNG export, and CPU reference tracing. `gpu` owns OptiX/CUDA resources through RAII; the device kernel rasterizes AO UV subpixels and casts cosine-weighted rays. GPU work is dispatched in bounded pixel/ray batches with cancellation checks between launches. `window` and `viewport` provide the asynchronous UI and OpenGL preview. `transfer`, `transfer_gpu` and `transfer_kernel.cu` implement texture reprojection on the CPU and on OptiX. They share their sampling code through `transfer_shared.h`. `transfer_panel` and `transfer_viewport` provide the Texture Transfer tab. Dependency details and licenses are in `THIRD_PARTY_NOTICES.md` and `licenses/`.
