# Smart fill

Smart fill repairs missing base-color paint in UV space after camera projections have
covered most of a mesh. It keeps every previously painted texel pixel-identical.
The AI supplies candidate colors; Fornaio controls exactly which native atlas texels
may receive them.

## Use Smart fill

1. Accept the camera projections you want to keep. Select the materials to repair in
   **Mesh, materials and generation settings**, then click **Smart fill...** beside
   the projection controls.
2. Review **UV texture and targeted gaps**. All missing paint in the selected
   materials is selected initially. Green marks the target regions. Click a region
   to exclude it; right-click to restore it. Switch materials to inspect each atlas.
   The preview shows **1 AI request** when any gaps are selected.
3. Click **Generate** once to fill all selected gaps in one image generation.
   Smart fill uses **GPT Image 2** through the existing Codex
   ChatGPT sign-in or OpenAI API route, even when the normal projection model is
   local Fibo. Fibo is not supported for this workflow. ChatGPT image limits or API
   billing apply to this single request. It does not start a sequence of crops.
4. Inspect the combined result in the UV tab and **3D candidate preview**. Orbit the
   mesh to inspect cavities and UV joins. Turn off **Show targeted gaps** to inspect
   the texture without the green overlay.
5. Click **Apply Smart fill** to accept the result as one UV repair layer and one
   undoable operation. The ordinary generated projection candidate, its captured
   camera, and existing layers are retained.

If no eligible gaps remain, no generation is needed. Region selection can be changed
before generation. After a result completes, keep, apply, or discard it before
choosing a different target mask.

## What counts as missing paint

Eligibility is **inside geometric UV occupancy AND unpainted**. Geometric occupancy
comes from the mesh's selected UV set at native atlas resolution, independently of
projection quality and texture padding. Empty space outside the islands is excluded;
one-pixel holes and missing texels touching island boundaries remain eligible.
Exterior padding is neither a repair target nor new surface coverage.

A fully unpainted island is eligible when another part of the same material already
has accepted paint to continue. A completely unpainted material needs an initial
camera projection first. Material selection keeps unrelated materials out of the
request and write mask.

Smart fill addresses missing coverage. It does not replace existing paint merely
because it looks weak, stretched, or incorrect. Use projection layers, masks, or
image replacement to improve those areas.

## One generation for all selected gaps

Smart fill sends the complete UV texture in one request. A single material keeps
its full native atlas as the input image. Multiple selected materials are arranged
in a deterministic grid within one image, capped at 4096 pixels square. The repair
map includes all selected regions across all materials, including tiny gaps.

The requested image is 1024 pixels square; returned sizes can vary by provider.
Fornaio maps its candidate colors back to each material's original atlas resolution.
This trades some potential repair
detail on larger atlases for one generation. The original atlas and write masks
stay at native resolution, so existing texture detail remains exactly unchanged.
Boundary color matching stays within the same material and UV chart.

Matching uses the accepted texture to anchor colors on the inside edge of each
hole. A smooth correction extends throughout the hole, retaining the generated
texture variations rather than fading back to a potentially wrong color after a
fixed distance. It never uses the AI's redrawn protected pixels as a color reference.
The method follows the gradient-domain approach in
[Poisson Image Editing](https://www.cs.jhu.edu/~misha/Fall07/Papers/Perez03.pdf),
with boundaries restricted to the editable side of the holes. An entirely blank
chart without accepted neighbors still depends on the generated candidate.

The request includes the texture image, a visible repair map, and a rendering of the
accepted textured object for appearance context. The prompt identifies all three
images and asks for local color and texture continuation while retaining UV layout
and avoiding new anatomy, lighting, or labels. The API route also sends an alpha edit
mask for the first image. Codex receives the visible repair map; it does not claim
structured edit-mask support.

The AI may redraw protected areas despite the prompt or mask. Fornaio discards all
returned pixels outside the frozen native repair mask. Only candidate colors are
resampled when the provider returns another image size. Boundary color correction
also writes inside holes only. Existing painted texels are never replaced with AI
output or feathered into it.

## Cancellation and saved projects

Accepted paint remains unchanged throughout generation. Cancellation or a failed
request does not start another generation automatically. Retry explicitly when
ready. A completed candidate can be previewed, applied, or discarded. Closing the
preview retains it; reopen **Smart fill...** to continue inspecting it.

Generation shows its current stage and elapsed time, rather than an estimated
percentage. **Processing Smart fill** and **Applying Smart fill** run in the
background and remain cancellable. A returned AI image is saved before matching;
if processing is cancelled or fails, **Retry processing** reuses that image without
another AI request. **Reprocess candidate** also recalculates matching from the
stored image without generating again.

Saving a `.fornaioai` project preserves the pending source snapshot, masks, material
layout, generated image, and source-state fingerprint. A pending repair is separate
from a pending camera-generation candidate.

Saved candidates from the earlier cropped workflow remain available for preview
and **Apply completed**. Their generation sequence cannot resume. Use **New
one-request preview** to replace that pending candidate with the single-generation
workflow; apply any old completed repairs first if you want to retain them.

If the mesh, UVs, atlas resolution, accepted paint, or relevant layer state changes,
the pending result stays available for preview but cannot be generated or applied
against the changed source. Use **Regenerate preview** to calculate a fresh
one-request plan from the current gaps. This replaces the old pending repair plan.

## UV repair layers

Applied Smart fill results appear as **UV repair** layers grouped beneath camera
projection layers. They bypass camera projection. Ordinary visible projection paint
is resolved first; repair pixels are used only where that stack remains empty.
A later projection therefore supersedes repair paint automatically. Synthetic repair
provenance and its low generation-guide confidence remain separate from ordinary
projection quality.

Repair layers support visibility, deletion, undo/redo, UV mask narrowing, and
**Regenerate Smart fill...** from the context menu. Camera mode, strength, depth
margin, camera masks, image replacement, and captured-camera controls are disabled
for them. Multiple visible repairs resolve in their displayed stack order, with the
upper repair taking precedence where their original masks overlap.

Hiding a camera projection does not expand a repair layer's original write mask.
Newly exposed gaps need a fresh Smart fill plan or regeneration. Regeneration
recomputes gaps with the selected repair excluded and replaces it only after the new
candidate is applied. Narrowing a repair mask cannot authorize painting outside its
original eligible area.

Smart fill works with legacy and current camera-projection backends without an
explicit projection rebuild. **Rebuild projection layers** preserves UV repair
layers. Projects containing repairs or a pending Smart fill use project format
version 3 and require a Smart fill-capable Fornaio build to reopen. Older version 1
and 2 projects remain readable. Save a separate project copy when retaining
compatibility with an older application build.

## Limits and validation

Exact preservation applies to existing atlas texels; generated appearance still needs
inspection. Texture continuation can fail in small cavities, on a blank island, or
across disconnected UV charts. Local boundary matching cannot guarantee a seamless
3D join across every UV seam, restore missing anatomical detail, or correct defects
already present in accepted paint. Inspect nostrils, ears, chin, eyelid interiors,
and UV joins in both views before applying.

`fornaio_smart_fill_tests` uses deterministic generated images to check eligibility,
native preservation, material/chart isolation, material-grid mapping, legacy partial
results, layer fallback, serialization, and cancellation. UI tests verify exactly
one provider result for separated gaps. These tests do not establish the
visual quality of a paid provider generation. See [validation evidence](../VALIDATION.md)
for separately recorded scene and provider checks.
