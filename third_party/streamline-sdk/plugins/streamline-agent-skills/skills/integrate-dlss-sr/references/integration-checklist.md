# DLSS-SR Integration Checklist

Use this checklist before and during code edits.

## Contents

- Required References
- Engine Discovery
- Functional Requirements
- DLSS Input Set
- Renderable Inventory
- Motion-Vector Convention
- Motion-Vector Implementation Options
- Jitter
- Output Extent and Composition

## Required References

- Target NVIDIA Streamline SDK package.
- Streamline documentation inside the SDK checkout.
- Project-specific SR checklist, bug notes, or gotchas supplied by the user, repository, or host environment.

## Engine Discovery

- Do not assume the engine structure.
- Locate the renderer, render graph or frame graph, RHI/backend layer, command submission path, swapchain/present path, post-processing chain, tonemap pass, UI/HUD pass, resource lifetime model, resize path, logging system, config/command-line system, and deterministic capture hooks.
- Choose the DLSS integration point where HDR scene color is available before tonemap/UI and where the output can feed the native-resolution post-DLSS pipeline.

## Functional Requirements

- The game builds successfully.
- The game runs with DLSS-SR disabled.
- The game runs with DLSS-SR enabled.
- With DLSS-SR enabled, internal render resolution is lower than output resolution and final presentation is at native output/window resolution.
- Add enough logging to prove internal render resolution, DLSS/output resolution, and final present resolution.
- Add a runtime flag, config option, command-line option, or debug toggle that best fits the project.
- Keep requested DLSS state separate from actual active DLSS state.
- If DLSS init/create/evaluate fails, clearly report fallback state and avoid claiming DLSS is active in UI/config/runtime logs.
- After Streamline evaluation, rebind any engine global state that Streamline or backend calls may disturb.

## DLSS Input Set

Ensure the game provides a complete DLSS-SR input set:

- HDR scene color before tonemap and before UI/HUD when the engine renders HDR.
- If the engine only generates SDR, note this explicitly.
- Depth.
- Motion vectors.
- Jitter offset.
- Reset flag.
- Render and output dimensions.
- Frame delta.
- Auto-exposure flag or fixed exposure values.

Trace frame delta through the whole stack: engine timing -> SR integration struct -> wrapper/plugin/RHI layer -> Streamline evaluation parameter. If any layer hardcodes, drops, defaults, or cannot expose frame delta, fix that layer or mark SR incomplete.

## Renderable Inventory

Before implementing DLSS-SR, inventory every renderable class and document whether each class writes valid motion vectors, intentionally writes zero vectors, is excluded from DLSS input, or needs a special mask/reset/handling path.

Include at least:

- Static meshes.
- Rigid moving meshes.
- Skinned or morphed meshes.
- Instanced meshes.
- Particles.
- Billboards.
- Foliage.
- Procedural vertex animation.
- Alpha-tested geometry.
- Alpha-blended geometry.
- Decals.
- Impostors.
- Terrain.
- Displacement or tessellation.
- Ray-traced primary hits.
- UI/HUD.

Do not assume zero motion vectors are acceptable for moving visual content. If no listed algorithm matches a renderable class, define a renderer-specific motion-vector path or explicitly exclude/mask/reset that class.

## Motion-Vector Convention

Validate explicitly:

- Resolution: render resolution or output resolution.
- Units: UV, NDC, pixels, or normalized.
- Direction/sign: current-to-previous or previous-to-current.
- Whether jitter is already included.
- Streamline motion-vector scale values.
- Jitter offset.
- Reset flag.
- Render dimensions.
- Output dimensions.
- Frame delta.

Inspect the shader/math path and confirm:

- Whether current clip positions include jitter.
- Whether previous clip positions include jitter.
- Whether jitter is already subtracted from the vectors.
- Whether `JitteredMotionVectors` or the equivalent SDK flag matches the actual convention.

DLSS-SR usually expects motion vectors to be consistent with the jitter offset passed to evaluate. If the MV convention and jitter flag do not match, fix the MV generation or the DLSS feature flags before validating image quality.

## Motion-Vector Implementation Options

Choose the smallest path that correctly covers the renderer.

- Depth reprojection: store current and previous view-projection matrices, reconstruct each pixel's current clip/world position from depth, project into the previous frame, and write previous_uv - current_uv into an RG16F motion-vector texture. This is a baseline for static-world and camera-induced motion vectors.
- Geometry velocity buffer: during geometry, compare current-frame position to previous-frame position using current and previous model-view-projection matrices. Static objects still output camera-induced motion; moving objects include object motion.
- Dynamic-object velocity pass: track previous transform state per entity/instance. Render moving objects into a motion-vector target with depth testing and combine with static camera/depth reprojection vectors.
- Skinned/animated velocity: preserve previous-frame bone matrices or interpolated vertex positions. Compute current and previous skinned clip positions in the velocity shader.
- 2D sprites, tile chunks, particles, and meshes: render into RG16F by comparing current screen-space quads/vertices with previous frame state. Track position, rotation, scale, camera matrix, layer/parallax transform, and animation frame where relevant.
- Mostly static 2D worlds: generate per-layer vectors from current and previous camera/view matrices using each tilemap/background/parallax layer's scroll factor.
- Animated visible pixels: for flipbooks, skeletal 2D rigs, particle simulation, UV scrolling, and shader deformation, preserve previous animation state and output vectors from previous visible pixel/vertex position to current position. At minimum, fast-moving particles and character sprites need object-level vectors or masking/reset to avoid ghosting.

## Jitter

- Use Halton or Sobol jitter, whichever fits the engine's existing temporal sampling.
- Verify jitter offset passed to DLSS matches the render projection and MV convention.
- Confirm reset behavior clears temporal history on startup, resize, toggle, large discontinuity, camera cut, and invalid history.

## Output Extent and Composition

- Verify post-DLSS pipeline writes the full output extent.
- Check viewport, scissor, tonemap dispatch, compute dispatch group sizing, fullscreen triangle/quad bounds, and any later fullscreen pass at native output resolution.
- Log and verify DLSS composition order: scene input before tonemap, UI/HUD after DLSS unless the engine intentionally does otherwise.
- If UI/HUD is intentionally rendered before DLSS, report the tradeoff and inspect for blur/ghosting.
