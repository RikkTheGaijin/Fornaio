# Projection blending

## Current default: GPT Image 2 with ownership (October 2026)

GPT Image 2 generates each camera view independently, so details (glasses, panel
seams, eyes) differ slightly between views and averaging views makes ghosts. The
default **Keep existing paint (paint around it)** mode therefore gives every texel
an owner, the way Modddif's inpainting multiview does:

1. **Locked guide.** Paint that an earlier view saw well (projection confidence of
   at least 0.1, eroded by two pixels at silhouettes and UV seams) is locked. The new
   view's guide shows the locked paint and camera-lit gray clay elsewhere; back faces
   are never rendered, so inner surfaces (a collar seen from behind) stay background.
2. **Generation.** GPT is told to paint only the clay and to use the reference for
   appearance only: its pose, tilt, camera and feature positions are ignored.
3. **Geometry alignment** (`aiAlignToGeometry`). Local normalized cross-correlation
   between the image's edges and the clay shading's edges (creases, slits, lip line,
   silhouette) gives shifts up to about 2% of the image, trusted only at clear peaks
   on real geometric structure; the smoothed field warps the image onto the mesh.
4. **Tone match** (`aiMatchLockedTone`). GPT also redraws the locked area; its gain
   against the accepted paint there (linear light, per channel, log domain, smoothed
   over about 4% of the image, capped at 1.6x) corrects the view's lighting/colour shift.
5. **Crossfade band** (`aiComposeLocked`, `aiGeneratedPaintMask`). Inside the lock's
   edge, a band 1/32 of the image wide fades from the tone-matched new paint to the
   accepted paint, and the projection strength ramps across it; the lock's core is
   excluded. Large bare-clay or white-background areas the model left unpainted are
   not projected (scattered gray pixels inside real gray paint are kept).

### Modddif's projection, measured from its public web client

- A view becomes a layer after a hard two-pixel erosion at the mesh silhouette
  (`dilateMaskPx: 2`); there is no blur or feathered mask. An optional per-layer
  depth-discontinuity margin drops pixels near depth jumps.
- Per texel, a view's weight is `alpha * dot(normal, view)^2` (discarded below 0.01,
  scaled by distance for perspective cameras), with a strict two-sided depth test.
- Blend layers accumulate premultiplied color and weight in floating point and divide
  at display: each texel is the cos²-weighted average of every view that saw it.
  Overlay layers use ordinary over-compositing; coverage is the maximum weight.
- Multiview inpaints only texels with coverage below 0.05 (about 77 degrees), from a
  render of the current texture, so overlapping views agree; a server-side color
  match (default strength 0) can correct the new region.

## Modddif research

The [official texture documentation](https://docs.modddif.com/project/texture/)
describes these behaviors:

- Multiview generation creates one projected layer per camera; single-view results can also become layers.
- Blend mode uses projection quality; overlay behaves like conventional image compositing.
- Layer strength above 100% increases influence on oblique surfaces.
- Projected masks and original cameras remain accessible.
- Color matching aligns generated images with existing texture.
- Layers support visibility, order, isolation, and external image replacement.
- Inpainting and patch painting repair local inconsistencies; hole filling expands nearby colors.
- Albedo mode reduces generated illumination. Default texture size is 2K, with 4K available.

The public page gives no exact quality or falloff formula. Fornaio's implementation
below is independent, inspired by those documented controls. Layer reordering and
an editable layer-stack compositor are beyond this change.

## Fornaio implementation

The default **Camera projection (repaint)** mode projects over accepted paint,
including fully covered meshes. Its opacity depends on the current camera angle,
strength, and feathered mask, without a comparison against previous confidence.
Near the camera-facing center the new image replaces existing paint; influence
falls off toward oblique surfaces and projection boundaries. Strength above 100%
increases the reach onto oblique surfaces. Generation uses confidence-weighted
feedback by default, independently of the projection blending mode. It receives
filtered accepted paint, normals, depth, silhouette, a reuse mask, prompt and user
references. A full first accepted image supplies a stable appearance reference,
separate from the camera-aligned paint guide. Hidden projections and projections
for other selected materials are excluded. Setting Keep previous details to 0%
restores independent neutral-geometry generation without this reference.
A fresh random seed is selected by default; fixed seeds remain available.

The optional **Blend by camera quality** mode records a 16-bit confidence atlas
separately from color and coverage. Its smooth quality comparison can suppress a
poorer new camera to protect stronger existing projections. It also feathers into
accepted overlap around newly covered texels. **Fill uncovered only** preserves
accepted texels and retains the previous narrow boundary correction. Generation
continuity is a separate setting and uses the same confidence filter in all modes.

Feathering uses a camera-space distance field at image, silhouette, mask, material,
and depth-discontinuity boundaries. Shared triangle edges are continuous. A
quintic falloff has zero first and second derivatives at its endpoints. Around
new coverage, a second feather extends into accepted overlap in camera space.
It follows the same visible surface across UV seams, with material, disconnected
surface, depth-discontinuity, and painted-mask barriers. Newly covered texels have
no old paint and use the incoming color directly. The adjoining overlap matches
that color at the join, then eases back to normal angle/strength opacity farther
inside existing paint. Both repaint and quality-blend modes use this transition;
its width is measured in camera pixels. This prevents the hard cutoff caused by
using full incoming color on one side and camera falloff immediately on the other,
including at reduced strength. Coverage is sampled from the current atlas through
the capture's frozen UV coordinates, so reusing a candidate does not reuse stale
coverage. Fill-only retains its UV-island-aware boundary correction.

Camera and UV rasterization evaluate shared edges consistently in double precision.
If a capture pixel belongs to another triangle, a native CPU BVH checks visibility
at the actual projected atlas sample. This handles multiple triangles within one
pixel without admitting hidden folds or thin occluders. Bilinear image/feather
samples follow the same connected surface and depth plane, rather than requiring
every tap to belong to an immediately adjacent triangle. Occluded, background,
and different-material taps remain excluded.

Optional color matching estimates a robust per-channel offset from corresponding overlap
texels, weighted by both cameras' confidence. Each offset is limited to 0.08 in
linear RGB. Blending is also in linear RGB, followed by sRGB encoding. This
corrects modest tone differences while retaining detail; it cannot reconcile
contradictory features or remove hallucinated geometry.

Defaults are camera repainting, 100% strength, 6% feather width, angle exponent 2,
and color matching off. Strength zero makes no changes. Repeated manual projections
remain available without adding coverage. Automatic repainting considers visible
camera influence instead of skipping covered areas.

Confidence, blend settings, and projection records survive saving and undo/redo.
Old projects load with neutral confidence if needed. The former 1.2.3 protected
default migrates to camera repainting with color matching off for future projections;
saved colors remain unchanged until a projection is accepted. Newly saved explicit
mode choices are preserved. Automatic quality blending considers improved camera
quality and new coverage; fill-only considers coverage. Projection and quality
assessment run outside the UI thread.

These changes require no additional model or runtime downloads.

## Camera and projection sharpness

New AI views use a 20-degree perspective lens. Changing FOV dollies the camera to
preserve center-plane framing; it is not an orthographic camera. The native capture
and OpenGL preview use the same lens. Frame mesh fits rotated mesh bounds inside
90% of the square image instead of using a fixed fraction of the bounding diagonal.
Automatic directions use the same lens and bounds fitting. Transfer's default
40-degree camera is unchanged. Linked viewports suppress identical camera
notifications so two-way camera synchronization cannot recurse.

The default maximum projection angle is 60 degrees from the viewing direction
to the surface normal. Grazing texels are rejected even on empty atlases instead
of being marked covered with stretched paint. On locally planar surfaces this
limits foreshortening expansion to roughly 2x; it does not impose a universal
pixel-density guarantee on arbitrary UVs or curved geometry. Influence eases to
zero over the last ten degrees before the cutoff. More camera views are needed
to cover the resulting holes. The limit is adjustable from 35 to 80 degrees;
strength cannot override it.

Camera matrices, lens, and projection limits are saved. Old cameras infer their
lens from their immutable projection matrix. Opening an old project narrows only
the live inspection camera for future views, preserving its framing. Existing
projection records, generated images and painted atlases are unchanged. Legacy
layer reconstruction without an explicit limit retains its former cosine .15
cutoff and falloff, avoiding changes to saved composition.

## Enclosed gap fill

**Fill enclosed visible gaps** is enabled by default for new projections. A
camera-space flood fill identifies empty regions enclosed by existing paint on
the same connected material surface. It samples live atlas coverage through the
capture's UVs, so reprojecting an old candidate sees the current coverage.
Image/silhouette, excluded-mask and unselected-material boundaries make a region
open; disconnected surfaces and depth jumps block traversal and cannot provide
paint support. Newly exposed outer regions keep the normal angle cutoff.

Only enclosed, visible, previously empty texels can use the current candidate at
angles between the selected maximum and 80 degrees. All visibility, material,
mask, and bilinear sampling checks still apply. This is image projection, not UV
dilation or copying an adjacent surface's colors. Previously accepted paint
outside the normal angle limit is unchanged. Fallback confidence is capped at
.075 independently of strength, leaving it free for replacement by a better
camera rather than treating it as trustworthy continuity detail. Automatic view
selection counts these repairable gaps. Coverage, confidence and paint operations
are recorded in the ordinary lossless layers and undo/redo history.

Projects save the checkbox and new projection records save its value. Legacy
layer reconstruction defaults to disabled, preserving old operations; opening a
project without the setting enables it only for future projections. The same
saved candidate can be projected again without running image generation. Hidden
surfaces, surfaces beyond 80 degrees, and holes connected to an outer boundary
still need a more suitable camera view.

## Generation continuity

### User appearance references

Reference-only requests use explicit, numbered appearance instructions. The
worker identifies each uploaded image after the geometry/reuse guides, asks for
its colors, pattern and material over the target surface, and identifies neutral
gray as an untextured placeholder. An optional text prompt adds appearance
instructions; blank or whitespace-only prompts need no user-written description.
The same numbering distinguishes the first accepted appearance view and fallback
material crop in later views. The reuse mask remains in latent anchoring and
post-generation detail preservation; it is not sent as an image in reference
requests, where its black regions could be mistaken for surface color. Trusted paint retains
its exact positions while new/weak regions receive the reference appearance.

For requests with user references, the worker converts the camera normal image
to an achromatic clay shape guide, retaining the exact foreground mask. Depth,
silhouette and the camera-aligned partial/neutral image remain separate inputs.
Raw normal-map hues are not sent as competing material colors. This conversion
is conditioning only; it is never projected into the atlas. Text-only requests
retain their existing normal input and prompt path. Reference images are read
with EXIF orientation applied, composited onto white through alpha, then fitted
inside 512 pixels. Original reference files are never modified.

UltraTex's [dataset inputs](https://github.com/yiboz2001/UltraTex/blob/main/ultratex/data/dataset.py)
separate multi-view normal guides from a rendered appearance reference. Its
[training path](https://github.com/yiboz2001/UltraTex/blob/main/train_flux2.py)
encodes each as separate image tokens and trains a texture adapter. Fornaio uses
the installed general Klein editor rather than those trained UltraTex weights.
The generic instruction to match unspecified references could produce a gray
copy of the geometry despite correctly loading the photo. Explicit material
assignment and color-neutral geometry address that failure in our editor. No
UltraTex code or weights are bundled by this change.

For continuity strength s in (0,1], the projection-aware guide includes reliable
previous confidence q through smoothstep(low, high, q), where low = .10 + .3(1-s)
and high = .40 + .5(1-s). This retains ordinary sharp overlap rather than only a
near-perpendicular core. A separate edge anchor uses low = .08 + .2(1-s), high =
.28 + .4(1-s), weighted toward low incoming projection influence. The original
stricter aiFeedback gate remains a lower-level compatibility helper; all reuse
contributions combine by maximum. Uncovered or nonvisible pixels receive zero weight.
The guide preserves the original RGB of qualifying paint instead of fading it
toward neutral gray. The percentage changes how much paint qualifies for reuse;
strong cores still receive full protection. Confidence describes previous
projection quality, not the new camera's angle.

The projection-aware guide also anchors sound previous paint where the incoming
view has little influence. It uses the new camera's normal angle and the same
silhouette/occlusion feather field as projection. This protects overlap edges
that would otherwise receive a different generated pattern. Neighboring paint
above a lower context threshold stays visible as spatial context even when its
reuse mask is zero; very poor paint and uncovered regions remain neutral.

Klein receives that filtered view and a separate reuse mask. It is instructed to
keep trusted detail positions and extend matching detail into gray regions. When
available, the first visible accepted view for a selected material supplies the
full-object appearance reference at up to 512 pixels. Its camera is explicitly
not the target camera. This avoids enlarging a small crop and suggesting a larger
grain/crack/chip scale. The projection ID is saved with candidate metadata; the
source image is retained in the projection stack. If no eligible accepted view
exists, a crop of strongly trusted material remains the fallback. Fresh and 0%
continuity requests exclude both forms of previous-paint appearance reference.

The pinned Klein image-editing pipeline has no native spatial denoising-strength
parameter. With the detail lock enabled, Fornaio adds masked flow-matching
denoising through the pipeline callback. The VAE encodes the camera-aligned paint
guide, and its mask is area-resampled to Klein's packed latent grid. After each
scheduler step, trusted tokens follow `(1-sigma)*reference + sigma*noise` at the
next scheduler sigma, using the original request's seeded noise. Uncovered tokens
are left free; partial confidence gives partial anchoring. This is an application
inpainting constraint, not a newly trained model. It uses the installed Klein
weights and adds no downloads. Turning off the detail lock disables latent anchoring.

After generation, new candidates use a smoothstep ramp over reuse mask values
128 through 255 to restore the source RGB. Full trust restores exact pixels;
weak context and zero-confidence areas cannot inherit paint. This keeps the
denoising ramp instead of cutting a new hard border at 224. Legacy commands
retain the former 224 core and 1.25-camera-pixel edge. Accepted paint is restored last in
those cores. Projection also protects the original atlas texels there,
avoiding camera/UV resampling of already good detail. Inherited source confidence
is preserved instead of replacing it with a weaker later camera's quality.

The worker returns the actual retained mask. It, the filtered guide, reuse mask,
material crop, and original source confidence are embedded with candidate/project
metadata. Reopening a pending candidate restores the confidence snapshot rather
than deriving its inherited quality from a later atlas. The warm process and model
weights are reused when continuity settings change. New regions can still differ
in pattern, tone or geometry; inspect candidates before accepting them. Previously
generated candidates are unchanged and must be regenerated to use the new guide
and denoising behavior.

## Coverage preview

The AI viewport optionally draws red/dark screen-space diagonal stripes wherever
the separate coverage atlas is empty. Nearest filtering without mipmaps or color
padding keeps the marker faithful to accepted coverage. Covered areas display the
selected unlit/lit texture normally; the explicit coverage view stays a binary
data preview. The hatch is only in the display shader, never in camera captures,
image conditioning, accepted projections, or exported PNGs. Texture Transfer opts
out by default. Coverage updates follow projections, undo/redo, and project loading.
