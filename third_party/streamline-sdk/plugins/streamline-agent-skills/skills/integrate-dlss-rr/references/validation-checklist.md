# DLSS-RR Validation Checklist

Use this checklist after wiring DLSS-RR. Do not stop after API integration.

## Contents

- Captures
- Capture Determinism and Reference Integrity
- Resolution Verification
- Path Confirmation
- Hard-Fail Conditions
- Integration Surface and Mode Matrix
- Required RR Input Validation
- Temporal and Derived Buffer Producer Chains
- Layered Guide-Buffer Validation
- RR Dev Debug Overlay
- RR Temporal Settle Validation
- Motion-Vector Validation
- Specular/Reflection Motion Strategy
- Test Harness Lifecycle Correctness
- Image Quality
- Regional Pixel Inspection
- Image Comparisons and Metrics
- Full-Output Write Validation
- Composition-Order Validation
- Runtime State Validation
- Resize and Toggle Validation
- Temporal Cache Interaction
- Existing Denoiser Interaction
- Final Report Template

## Captures

Capture deterministic frames for:

- RR disabled / existing renderer path.
- No denoiser / noisy path-traced input, if practical.
- Existing denoiser, if present.
- DLSS-RR enabled.
- High-sample or accumulated reference, if practical.
- At least one moving-camera or moving-object sequence, not only still images.

Comparable-capture requirements:

- Use the same scene, camera path, resolution, exposure/tonemap settings, random seed or deterministic sampling setup, and frame index/window timing wherever practical.
- Capture all comparable modes for every RR integration: noisy input, existing denoiser, DLSS-RR, and reference when those modes exist.
- If a mode cannot be captured, explain why and do not silently omit it from the comparison matrix.
- Keep RR disabled/existing-renderer captures for functional regression checks, but do not treat them as denoising-quality references when they use a different renderer path.
- For videos, prefer simple side-by-side artifacts first: noisy, existing denoiser, and DLSS-RR on the same camera path. Add difference views or stacked diagnostics only when needed for debugging.

## Capture Determinism and Reference Integrity

For validation harnesses, automated captures, or manual reproducibility:

- Log the exact mode selector, command-line arguments, config files, runtime toggles, output path, scene, camera, exposure, resolution, sample count, and random-seed policy used for each capture.
- Write each mode to an isolated output path or include the mode and timestamp in the filename. Do not allow RR, noisy, existing-denoiser, and reference captures to overwrite each other.
- Record hashes, image dimensions, and modification times for the images that will be compared.
- For high-sample or accumulated references, confirm the renderer reached the requested sample count in logs before saving the reference image. Do not compare against an incomplete or stale reference.
- Verify the same scene, camera, animation time, exposure/tonemap settings, and present resolution before computing metrics.
- If a reference or comparison image contains stale regions, overlays, camera mismatch, incomplete accumulation, or content that is not present in the other captures, report it and avoid overclaiming full-image metrics.

## Resolution Verification

Print and verify resolutions for each run:

- Internal/render resolution.
- RR input resolution.
- RR output/native resolution.
- Final window/swapchain/present resolution.

## Path Confirmation

Confirm:

- With RR disabled, the renderer follows its normal path.
- With RR enabled, RR receives noisy HDR ray-traced/path-traced input before tonemap and before the existing denoiser.
- RR output is native/output resolution.
- Final present resolution is native/output resolution.
- DLSS-SR is not run as a second pass after RR.
- Dynamic resolution is disabled, rejected, or cleanly falls back while RR is active.
- Fullscreen/composite passes after RR use native/output viewport, scissor, dispatch extents, and framebuffers.

## Hard-Fail Conditions

Treat these as integration failures, not cosmetic issues:

- RR reads accumulated color history instead of fresh noisy current-frame input.
- RR input or guide resources have missing/invalid producer write barriers or SDK read/write states.
- Jitter used to generate color/guides does not match jitter passed to RR.
- Primary motion vectors have unknown sign/units/scale.
- Moving objects produce zero/stale motion vectors and this is not documented as an MVP limitation.
- Required guide buffers are uninitialized, stale, wrong resolution, wrong format, or mostly default values in valid surface regions.
- RR evaluates after tonemap, after UI, or after an existing denoiser without explicit justification.
- RR requested state is reported as active after support/create/evaluate failure.

## Integration Surface and Mode Matrix

Record the integration surface:

- Direct Streamline integration.
- NVIDIA Streamline.
- Engine plugin or local wrapper.
- Existing DLSS-SR/DLAA path reused only for shared setup, not for RR evaluation.

Validate every exposed mode where present:

- Disabled/noisy path.
- TAA or engine AA.
- DLSS-SR/DLAA.
- Existing denoiser.
- DLSS-RR.

For each mode, confirm requested state, actual active state, fallback destination, input/output resolution, and whether existing denoisers, TAA, temporal lighting caches, frame generation, UI/HUD composition, and post-processing histories are active or bypassed.

## Required RR Input Validation

Validate required RR inputs:

- Noisy HDR ray-traced/path-traced color.
- Depth.
- Primary motion vectors.
- Normals.
- Roughness.
- Diffuse albedo.
- Specular albedo.
- Output target.
- Specular/reflection motion vectors OR specular hit distance plus required matrices.

For each RR input, log and inspect:

- Resource name.
- Format.
- Resolution.
- Viewport/scissor coverage.
- Clear value.
- Color space.
- Resource state/barriers.
- Whether sky/background pixels are valid.
- Whether all renderable classes write valid data or are intentionally excluded/handled.
- Whether optional inputs are null/omitted, valid real resources, or intentional dummy resources.

## Temporal and Derived Buffer Producer Chains

For every required temporal or derived RR input, especially motion vectors, do not validate only the final buffer. Also validate the producer chain that creates it.

For each derived buffer, identify and log:

- Upstream source buffers, IDs, or visibility data used to generate it.
- Shader/pass that writes it.
- Frame/update phase when source data is produced.
- Whether current-frame and previous-frame state are both valid.
- Whether dirty/change flags, reset flags, or update requests are consumed by the normal engine update path.
- Whether the test harness bypasses, manually clears, or precomputes state that the renderer normally expects to update itself.

If a required buffer is zero, sparse, stale, or suspicious:

- First verify that upstream source data is present and nonzero.
- Then verify that the pass consuming that source data actually dispatches.
- Then verify that current and previous state differ when motion is expected.
- Then verify that the output buffer changes after the dispatch.
- Do not assume the required buffer is missing just because the final buffer is zero.

Classify failed RR inputs as one of:

- Missing upstream source data.
- Upstream source valid but producer pass not running.
- Producer pass running but previous/current state invalid.
- Producer pass running but output write invalid.
- Test harness lifecycle issue.
- Real renderer integration issue.
- Unsupported/unimplemented content class.

## Layered Guide-Buffer Validation

For renderers that synthesize RR inputs from path-space decomposition, stable planes, visibility buffers, ray-hit data, or layered denoising buffers:

- Capture or visualize noisy color, stable/non-noisy radiance, layer weights, selected primary/dominant layer, normals, roughness, diffuse albedo, specular albedo, primary motion vectors, specular motion vectors, and specular hit distance where present.
- Validate sky/background, emissive-only pixels, no-surface pixels, missing secondary layers, perfect specular paths, rough specular paths, transparent/refractive paths, and invalid ray hits.
- Verify guide normals are normalized and in the expected space.
- Verify roughness/albedo repair or minimum-guide logic does not hide missing material data.
- Verify recomposed RR input color remains HDR pre-tonemap and follows the renderer's pre-exposure convention.
- Document any brightness clamp, local tonemap, minimum albedo, layer-mixing, or dominant-layer heuristic used only for RR.

## RR Dev Debug Overlay

If the RR dev debug overlay is available, inspect:

- Noisy input color.
- Primary motion vectors.
- Depth.
- Jitter offset history.
- Normals.
- Roughness.
- Diffuse albedo.
- Specular albedo.
- Specular motion vectors or specular hit distance path.
- Optional transparency/color-before-transparency/SSS/DOF guides if implemented.

## RR Temporal Settle Validation

For RR captures and metric runs:

- Do not save the first evaluated frame after RR enable, reset, resize, preset change, scene load, camera cut, or target sample-count transition.
- Wait for a documented number of evaluated frames after the input signal is ready. Choose enough frames to cover the renderer's temporal history and RR history, then log the settle count used.
- Record reset flag, frame delta, jitter offset, MV scale, render/output dimensions, and RR evaluate status for the saved frame.
- Sanitize startup, pause, debugger, shader-compile, or loading-hitch frame deltas before RR evaluate. If a fallback delta is used, log it and explain why.
- For path-traced accumulation, distinguish accumulation sample count from RR temporal settle frames. A target SPP image can still need extra RR evaluations before a representative capture.
- For accumulated renderers, verify RR receives fresh current-frame noisy color and matching current-frame guides, not accumulated renderer history, and verify RR history is preserved across ordinary camera/object motion.
- When validating temporal stability, inspect a sequence of frames instead of only a single saved image.
- Static-scene expectation: after temporal settle, RR and the existing denoiser should be visually comparable to the eye, though RR may preserve different detail and should be judged against a common reference when available.
- Motion-scene expectation: during camera or object motion, RR should stay temporally stable because it owns its temporal reconstruction history; spatial/offline denoisers may remain noisy, become unavailable, or only become clean after motion stops and accumulation settles.

## Motion-Vector Validation

Validate motion vectors with:

- Static camera / static scene.
- Fast camera pan.
- Moving object with static camera.
- Moving camera + moving object, if practical.
- Skinned/animated object, if present.
- Instanced object, if present.
- Particles/sprites/billboards, if present.
- Reflective moving object or reflected moving object, if present.

For each MV case, read back and log:

- Primary MV buffer stats.
- Upstream primitive/object/visibility/depth source stats, if used by the MV pass.
- Previous/current transform or previous/current state validity.
- Whether object/scene dirty flags were set before renderer update.
- Whether those dirty flags were consumed by the normal scene/render update.
- Whether the validation harness altered transforms, animation, camera state, or resources in a way that bypasses normal engine lifecycle.

Validate MV convention explicitly:

- Render-res or output-res.
- UV/NDC/pixel/normalized units.
- Current->previous or previous->current sign.
- Jittered or unjittered matrices.
- Jitter offset passed to RR.
- Streamline MV scale values.
- Reset flag.
- Frame delta.
- MV scale constants used by Streamline or the project wrapper.

## Specular/Reflection Motion Strategy

Validate the active specular/reflection motion strategy:

- Specular/reflection motion vectors.
- Specular hit distance plus required matrices.
- Wrapper-specific fallback, if either resource is missing.

If both strategies exist, prove only the selected strategy is tagged/evaluated. Inspect reflective and refractive cases, curved reflectors, moving reflected objects, moving reflector objects, roughness-threshold behavior, missing hit distances, and camera-only reflection motion.

## Test Harness Lifecycle Correctness

The test harness must exercise the same update path used by the real renderer.

Validate that:

- Scene/camera/animation changes happen before the engine update that consumes them.
- Per-frame animation leaves the appropriate dirty/change state visible to the renderer unless the engine explicitly requires manual updates.
- One-time setup is separated from per-frame animation.
- Manual setup does not hide changes from previous/current tracking.
- The harness does not manually update transforms, cameras, animations, acceleration structures, previous-frame buffers, or dirty flags in a way that bypasses normal engine lifecycle.

## Image Quality

Inspect image quality for:

- Ghosting/trails.
- Reflection lag.
- Specular smearing.
- Disocclusion artifacts.
- Unstable denoising.
- Edge shimmer.
- Missing specular detail.
- Particle/transparency disappearance or ghosting.
- UI/HUD blur or ghosting.

## Regional Pixel Inspection

Perform regional pixel inspection:

- Bottom rows.
- Right columns.
- Corners.
- Center crop.
- Max-error pixels.
- Black/zero pixels where reference/native is nonzero.

## Image Comparisons and Metrics

Compare output images where meaningful. For every RR integration, produce a comparison matrix using all captured comparable modes:

- RR vs high-sample/accumulated reference, if practical.
- Existing denoiser vs high-sample/accumulated reference, if an existing denoiser is present and a reference exists.
- Noisy input vs high-sample/accumulated reference, if a reference exists.
- RR vs existing denoiser, if an existing denoiser is present.
- RR vs noisy input.
- Existing denoiser vs noisy input, if useful for context.
- RR disabled vs RR enabled only as a functional/regression comparison; do not use it as a denoising-quality metric if the disabled path uses a different renderer.

For each comparison pair, state exactly what image A and image B are. Do not let labels like `RR vs reference` remain ambiguous.

Compute objective metrics where practical:

- MAE.
- MSE.
- PSNR or SSIM, if available.
- Regional MSE for edges/corners/center crop.
- Regional MAE for edges/corners/center crop.
- Max RGB distance or max-channel absolute error, if practical.
- Explicitly state whether MAE and MSE are computed on raw `[0,255]` values or normalized `[0,1]` values. Prefer raw `[0,255]` reporting unless the user requests normalized metrics. If normalized metrics are computed internally, also report the `[0,255]` equivalents as `MAE_255 = MAE_normalized * 255` and `MSE_255 = MSE_normalized * 255 * 255`.

Use common-reference comparisons to judge denoising quality. For example, compare RR vs reference, existing denoiser vs reference, and noisy input vs reference. Direct RR vs existing-denoiser only shows how different the two outputs are; it does not by itself prove which one is more correct.

For metric reporting:

- Produce a side-by-side or contact-sheet image for the compared captures when practical.
- State whether RR beats the existing denoiser against the same reference for MAE and MSE, and include the margin when the difference is small.
- Report regional metrics when a full-image metric could hide edge, center, history, UI, or stale-pixel artifacts.
- If image smoothness and metrics disagree, do not treat smoothness as correctness. Inspect detail preservation, temporal stability, and guide-buffer validity before changing the integration.

Do not rely only on MSE. Treat temporal stability, reflection behavior, denoising quality, correct buffer conventions, and producer-chain correctness as equally important.

## Full-Output Write Validation

- Inspect final row/column after RR.
- Inspect final row/column after tonemap.
- Verify no later pass uses internal-resolution viewport/scissor/dispatch against output-resolution targets.
- Verify no black borders, unwritten pixels, or stale history pixels.

## Composition-Order Validation

- Confirm tonemapping runs after RR.
- Confirm UI/HUD/text/menu overlays run after RR unless intentionally documented otherwise.
- Confirm Depth of Field runs after RR unless using RR Preset E with a valid DOF guide.
- Confirm bloom, exposure, sharpen, TAA, and other post effects do not incorrectly assume render resolution after RR.

## Runtime State Validation

Test runtime states:

- RR requested but unsupported GPU/backend.
- Missing required Streamline DLSS-RR plugin.
- Streamline support-query failure, if practical.
- Streamline feature-creation failure, if practical.
- Streamline evaluation failure, if practical.
- Streamline/plugin availability, option-setting, resource-tagging, or evaluate failure, if applicable.
- Engine wrapper fallback path, if applicable.
- Required RR buffer missing or invalid.
- Dynamic resolution requested while RR is active.

Confirm actual active RR state falls back cleanly and logs why.

## Resize and Toggle Validation

Validate:

- RR off -> RR on -> RR off.
- Native size -> resize window -> RR still correct.
- RR enabled during resize.
- RR preset/mode changes recreate or resize resources correctly.
- Repeated toggles do not leak resources or leave stale feature handles.

## Temporal Cache Interaction

- Verify temporal caches reset on RR off -> on, RR on -> off, resize, preset/quality changes, camera cuts, and render/output resolution changes.
- Verify existing denoiser histories are bypassed while RR is active and still valid when RR is disabled again.
- Verify temporal lighting/sampling systems such as ReSTIR DI/GI, lighting reservoirs, exposure history, frame generation, and history clamps are disabled, reset, or validated while RR is active.
- Verify UI/HUD and frame-generation paths consume post-RR color at the intended stage and do not feed UI into RR.

## Existing Denoiser Interaction

- Verify existing denoiser can still run when RR is disabled.
- Verify RR path bypasses the existing denoiser unless intentionally documented.
- Verify no double-denoising unless explicitly justified.

## Final Report Template

Include:

- Files changed.
- Integration quality level: MVP, production candidate, or reference quality.
- MVP hard-gate pass/fail table.
- Build command.
- Run command with RR disabled.
- Run command with RR enabled.
- Runtime flag/config/menu used.
- Integration surface and mode matrix results.
- Printed resolutions.
- RR requested state vs actual active state.
- RR input buffer table with format/resolution/status.
- Resource-state/render-graph audit summary for every RR input and output.
- Producer-chain validation summary for temporal/derived buffers.
- Whether RR input color/guides are fresh current-frame buffers or accumulated renderer history, and how RR history is kept separate from renderer accumulation.
- Layered guide-buffer synthesis summary, if applicable.
- Motion-vector convention summary.
- Motion-vector producer-chain summary.
- Specular/reflection motion handling summary.
- Image comparison matrix results, with each pair explicitly defined.
- Side-by-side/contact-sheet artifact path, if produced.
- MAE/MSE scale used (`[0,255]` or `[0,1]`) and conversion if applicable.
- Regional pixel findings.
- Fallback/error-path results.
- Resize/toggle results.
- Temporal cache and mode-interaction results.
- Guide-buffer known limitations, including sky/background, transparent content, particles, skinned meshes, decals, and specular/reflection handling.
- Follow-up PR list, separated into correctness fixes, quality improvements, validation improvements, and optional reference-quality work.
- Any known incomplete, fragile, or counterintuitive parts.
