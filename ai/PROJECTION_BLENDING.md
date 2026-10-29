# Projection layers

## Current behavior

New AI Texturing projects use independent projection layers modeled on Modddif's
public client. Each accepted camera image is projected once into an RGBA8 UV
image. The visible stack is composed from those images in Float32; earlier paint
does not acquire ownership of atlas texels. The default is **Blend**, **100%**
strength, and **Depth margin 2**.

Fornaio keeps its GPT Image 2 and optional local Fibo providers, prompts, camera
grid, and generation guides. Matching the projection client does not reproduce
Modddif's private image-generation service, and does not guarantee consistent
details between independently generated images.

The provider's original image is embedded separately from the editable camera
mask, UV mask, and derived UV projection. The parity projection path does not
apply Fornaio's former geometry-registration warp, automatic tone match,
locked-core restoration, lock-edge crossfade, angle cutoff, gap fill, or
paint-every-face fallback. Generation continuity remains a separate provider
input; it is not a projection ownership rule.

## Reference and projection contract

The implementation was checked against the public
[Modddif client bundle](https://app.modddif.com/assets/main-BZiTw8uP.js), observed
on October 29, 2026. Its SHA-256 is:

```text
f5826dcf78e6215d72841cbcca0a47e8afa9d6768a70f55d321249477615b9f2
```

The [official texture documentation](https://docs.modddif.com/project/texture/)
describes projected layers, Blend/Overlay, strength, masks, order, isolation,
camera recall, and image replacement. The following numerical behavior comes
from the client shaders rather than an assumed visual approximation.

### Camera image and visibility

- Render front-side geometry into a Float32 camera-depth image. Images and
  imported source alpha retain their original framing.
- Erode the foreground silhouette with a hard 5 by 5 square neighborhood:
  two source-image pixels on each side. No blur or feather is introduced.
- Filter source images with premultiplied alpha, then unpremultiply the sampled
  RGB before storing the projected layer. This prevents transparent background
  RGB from contaminating texture edges.
- Rasterize UV geometry double-sided. Source coordinates outside the camera
  image, fully transparent source samples, and excluded masks are rejected.
- Use normalized depth `z = clipZ / clipW * 0.5 + 0.5`. Subtract a normal-based
  bias of `0.00002 * dot(normalize(normal), direction)` for orthographic
  cameras, or `0.000001 * dot(normalize(normal), direction)` for perspective.
  Accept only `depth - 0.0001 <= z <= depth`.
- **Depth margin** maps to a neighborhood radius of `ceil(margin / 2)`.
  The default 2 therefore checks a 3 by 3 neighborhood; a normalized depth jump
  greater than 0.001 rejects the sample. Zero disables this extra margin.

The native renderer uses a dedicated offscreen OpenGL 3.3 context on its own
thread, independent of the preview. It explicitly disables multisampling,
framebuffer sRGB conversion, and dithering, and does not inherit the preview's
mipmaps, anisotropy, or repeat wrapping. Projection failure leaves accepted
layers intact; the legacy CPU algorithm is not silently substituted.

### Projection weights and composition

For Blend, start with `s = dot(interpolatedNormal, directionToCamera)`.
Normals are normalized per vertex, then interpolated. Reject `s < 0.01`
**before squaring**. Multiply `s` by
`smoothstep(0, 0.5, 1 / clipW)`; the projected alpha is then
`sourceAlpha * s * s`, multiplied by the editable masks. Orthographic
projections have no perspective distance attenuation. Overlay bypasses
similarity weighting.

This is not a hard 60-degree cutoff. For example, orthographic 60-, 70-, and
80-degree surfaces have approximately 0.25, 0.117, and 0.030 weight before
RGBA8 quantization. A low-weight single view can still show stretched detail;
another suitable camera is needed to provide sharper information.

Each projected UV image stores encoded RGB and alpha in RGBA8. Strength
multiplies alpha during Float32 composition. Blend adds premultiplied RGB and
weight; Overlay attenuates the existing accumulator and adds its premultiplied
contribution, with strength capped at 100% for Overlay. Display and export divide
the RGB accumulator by its weight; accumulated alpha at or below 0.001 is empty.
Pure Blend stacks are order independent within floating-point tolerance; mixed
stacks with Overlay depend on order.

Composition intentionally weights source RGB values without sRGB-to-linear
conversion, matching the public client. Independent maximum layer weight is
retained as Float32 coverage quality, including Blend strengths above 100%.
The existing 16-bit confidence image is a clamped adapter for generation guides.

Padding searches the nearest geometric UV-island texel within a five-pixel
radius and copies it only outside islands. It does not fill missing paint inside
an island. Its source is geometric island coverage, not the nearest painted
texel.

Two earlier research notes were inaccurate: the 0.01 rejection applies to the
cosine before squaring, not the final weight; and 0.05 is an explicit inpaint
fallback threshold, not Modddif's ordinary multiview generation cutoff. The
public multiview regeneration mask is
`1 - smoothstep(0.1, 0.7, maxCoverage)`. This observation does not change
Fornaio's provider-specific generation strategy.

## Editing layers

The **Projection layers** panel lists the stack newest first. Select a layer to
change **Mode**, **Strength**, or **Depth margin**, then click **Apply layer
settings**. **Move up** and **Move down** change stack order. **Isolate selected
layer** temporarily composes that layer for inspection.

**Camera mask** and **UV mask** open mask editors: left drag excludes paint,
right drag restores it, and white keeps paint. **Replace image...** accepts a
square image with the original captured framing; **Go to captured camera**
returns the inspection viewport to that layer's camera without changing its
stored projection.

**Delete layer** is available as a button and a context-menu action. It removes
the selected layer immediately, without a confirmation dialog; **Undo** restores
it. Deleting an isolated layer clears isolation. Deleting the final layer restores
the base paint. Referenced mesh and external image files are never deleted.

Visibility, order, isolation, strength, and deletion recompose saved UV
contributions. Mode, mask, depth-margin, and image changes rebake the affected
layer from its saved camera and source image. These operations require no AI
generation. Preview, coverage, future generation feedback, and exports use the
resulting stack. Controls are disabled during processing.

Layer identities are separate from generation request IDs. Composition caches
include content, settings, backend, order, visibility, and isolation so edits
cannot reuse an unrelated cached composition.

## Existing projects and rebuilds

Opening an older project preserves its saved appearance and legacy projection
behavior. Its frozen version-1 UV operations are replayed by the legacy
compositor until **Rebuild projection layers** is selected. Legacy angle,
feather, gap-fill, repaint, fill-only, and ownership settings remain compatibility
behavior; they do not control parity layers.

Rebuild projects every stored layer, including hidden layers, into independent
version-2 UV assets and changes the stack to the new backend. It preserves layer
order and visibility and keeps existing base paint as the bottom UV layer.
Legacy projections adopt Blend, 100% strength, and depth margin 2 during this
explicit conversion. The complete result is committed as one undoable
operation. Cancellation or an error preserves the previous stack and backend.

Old projects usually contain only the processed candidate image and combined
mask. Rebuilding can correct their projection and composition, but cannot undo
alignment, tone changes, restored old paint, or ownership exclusions already
baked into those images. Replace a layer image when an unprocessed original is
available. New projects retain the provider's original source separately.

Saving preserves the backend, base paint, sources, masks, independent UV assets,
layer settings, and visibility. Hidden layers retain their contributions for
later editing. Deleted layers are absent from subsequently saved projects but
remain available in the current session's undo history.

No new AI models, runtime downloads, or paid generations are needed to rebuild
or edit projections. Mesh files must remain available and unchanged.

## Verification

`tests/projection_reference.html` is a standalone offline WebGL2 pixel oracle.
Its observed results are in `tests/projection_reference_golden.json`, including
the browser and GPU renderer identity. The fixtures cover projection weights,
premultiplied alpha, masks, erosion, depth checks, all default discontinuity
neighbors, UV padding, and Float32 composition.

`fornaio_projection_tests` exercises the native public renderer and compositor
against those fixtures and additional geometry cases. Interior RGBA8 comparisons
allow one channel level; Float32 comparisons allow 0.000001. Raster boundaries
are checked separately rather than claiming cross-driver bitwise identity.
Existing AI and UI regressions continue to cover legacy projects and layer
history. Actual test runs and saved-head comparisons are recorded in
`VALIDATION.md`.

`fornaio_ai_eval project` uses independent parity layers; `legacyproject` and
`ownproject` retain the older evaluation paths. `replay <project.fornaioai>
<output-directory>` rebuilds a copy from saved images and writes before/after
renders and metrics without inference. The original project is not overwritten.

The uncovered hatch remains a preview overlay and is never projected or
exported. New views are still needed for missing or poorly observed geometry,
and generated images can contain lighting, feature drift, or inconsistent
details that projection alone cannot repair.
