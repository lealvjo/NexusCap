# DLSS-RR Integration Checklist

Use this checklist before and during code edits.

## Contents

- Required References
- Engine Discovery
- API or Wrapper Discovery
- Core Requirements
- MVP Hard Gates
- Resource State and Render-Graph Audit
- DLSS-RR SDK/API Requirements
- DLSS-RR Input Set
- Guide Buffer Anti-Patterns
- Path-Traced, Stochastic, and Temporal Inputs
- Accumulated Renderer and RR History
- Optional Guide Buffers
- Layered or Path-Space Guide Generation
- Renderable Inventory
- Motion-Vector Convention
- Motion-Vector Producer Chain
- Motion-Vector Implementation Options
- Specular and Reflection Motion
- Sampling and Noise
- Temporal Systems and Cache Interaction
- Logging
- Post-RR Validation

## Required References

- Target NVIDIA Streamline SDK package.
- Streamline documentation inside the SDK checkout, especially the DLSS-RR guide material.
- Project-specific RR checklist, bug notes, or gotchas supplied by the user, repository, or host environment.

Read the DLSS docs and the DLSS-RR guide before editing code. Do not assume the engine structure.

## Engine Discovery

- Inspect the renderer and choose the correct integration point.
- Locate the ray/path-tracing path, existing denoiser path, render graph or frame graph, RHI/backend layer, command submission path, swapchain/present path, post-processing chain, tonemap pass, UI/HUD pass, resource lifetime model, resize path, logging system, config/command-line system, and deterministic capture hooks.
- If no per-engine addendum exists, create a temporary engine profile from source inspection before editing. Include: build system and target, executable/run command, graphics APIs, render graph or frame graph owner, backend/RHI abstraction, Streamline wrapper location, path-tracing pass, existing denoisers, G-buffer or path-space guide producers, motion-vector producer chain, scene loading, camera control, screenshot/video capture, output directory policy, and test scenes.
- Prefer deriving engine-specific steps from checked-in scripts, CI files, CMake/project files, launch configs, sample apps, editor command-line handlers, and existing validation tools. Do not require the user to provide per-engine notes unless the repository genuinely lacks enough information to build or run.
- If DLSS-SR or another NVIDIA upscaler already exists, inspect it only for reusable Streamline device and resource-management patterns. Keep requested/active SR state separate from requested/active RR state.
- If the project has an AA/SR/denoising mode selector, map every mode and decide where RR belongs. RR should be mutually exclusive with standalone denoisers and should not be followed by a separate SR pass.
- Locate temporal systems that may use the same inputs or histories as RR: TAA, frame generation, NRD, ReSTIR/RTXDI, lighting caches, exposure, history clamps, reactive masks, and capture/reset code.

## API or Wrapper Discovery

- Determine whether the project integrates DLSS through NVIDIA Streamline, an engine plugin, or a local Streamline wrapper. Use the existing project abstraction where practical instead of replacing it with another API layer.
- For Streamline or a wrapper, inspect the wrapper implementation and verify it exposes all RR-specific behavior: support query, runtime/plugin availability, per-frame constants, DLSS-RR options, resource tagging, evaluate call, cleanup/recreate path, and logging.
- Do not treat DLSS-SR resource tagging or evaluate calls as sufficient for RR. RR needs its own color, depth, motion, normal/roughness, albedo, specular/reflection-motion or hit-distance inputs.
- Verify the runtime package includes the required Streamline DLSS-RR plugin and that unsupported or missing components fall back without claiming RR is active.
- If the wrapper has ambiguous parameter order or overloads, trace the implementation to prove which resource is input color and which is output/native color.

## Core Requirements

- The game/application builds successfully.
- It runs with DLSS-RR disabled.
- It runs with DLSS-RR requested/enabled when supported.
- Keep requested RR state separate from actual active RR state.
- If support query, init, create, evaluate, or required-buffer setup fails, clearly log fallback state and do not claim RR is active.
- Add a runtime flag, config option, command-line option, menu option, or debug toggle that enables/disables RR for testing.
- If adding a validation harness, include comparable capture modes for noisy input, existing denoiser, DLSS-RR, and high-sample/accumulated reference where practical. The harness should compute a comparison matrix for every integration, including RR vs reference, existing denoiser vs reference, noisy input vs reference, RR vs existing denoiser, and RR vs noisy input when those modes exist.
- If no validation harness exists, implement a minimal one instead of relying on manual screenshots. Required minimum: deterministic scene load, fixed resolution, fixed exposure or tonemap settings, fixed camera transform/path, mode-specific output filenames, logs for requested/active denoiser state, and repeatable command-line/config toggles for noisy, existing denoiser, RR, and reference when practical.
- If the renderer has an existing denoiser, add runtime modes for at least:
  - no denoiser / noisy path-traced input, if practical
  - existing denoiser
  - DLSS-RR
- If the renderer already exposes AA/SR choices, include RR as a clear mode in that same surface when practical, alongside disabled/noisy, TAA, DLSS-SR/DLAA, and existing denoiser choices.
- DLSS-RR must consume the noisy ray-traced/path-traced input before existing denoising stages unless a different placement is explicitly justified.
- DLSS-RR must run before tonemapping and before most post-processing, as close to the start of post-processing as practical.
- Do not run DLSS-SR as a second pass after RR.
- Depth of Field must run after RR unless using RR Preset E with the required DOF guide.
- Dynamic resolution must be disabled, rejected, or cleanly fallen back when RR is active because RR does not support DRS.
- After Streamline evaluates, rebind any engine global state that Streamline or backend calls may disturb.
- After Streamline or an engine wrapper evaluates RR, clear/rebind global graphics or compute state if the backend may disturb descriptors, pipelines, root signatures, viewports, scissors, or barriers.
- When render/input resolution differs from output/native resolution, ensure all fullscreen passes after RR use output/native viewport, scissor, dispatch extents, framebuffers, and composite views.
- Before finalizing, perform a code-review pass and call out anything unusual, fragile, incomplete, or counterintuitive.
- Specifically review whether validation harnesses, debug modes, or integration code manually update transforms/cameras/animations/resources in a way that bypasses normal previous/current state tracking.

## MVP Hard Gates

An RR integration is not acceptable, even as MVP, unless all of these are true:

- RR input color is fresh current-frame noisy HDR ray/path-traced color, not accumulated renderer history and not output from an existing denoiser.
- RR runs before tonemap, UI/HUD, and most post-processing.
- All required RR guide buffers are present at the expected resolution and are current-frame coherent with the color input.
- Every guide producer writes through valid resource states/layouts/barriers before RR reads the resources.
- The SDK/Streamline evaluate call reads inputs in shader-resource/readable states and writes output in UAV/renderable/write state, with later engine passes using restored or valid states.
- Motion-vector convention is known: units, sign, jitter inclusion, scale, resolution, and current/previous frame ownership.
- A moving-camera test and at least one moving-object test have been run or the missing object-motion support is explicitly reported.
- Jitter passed to RR matches the jitter used to generate the noisy color and guides. Projection jitter, ray jitter, and post-process jitter must not be double-counted.
- RR requested state and actual active state are separate, logged, and fallback-safe.
- Resize and RR on/off toggles recreate or validate feature handles, resources, histories, and reset flags.

## Resource State and Render-Graph Audit

Validate resource states for the full producer-consumer chain, not only the SDK evaluate call.

For every RR input and output, inspect:

- Allocation/default layout.
- Clear pass state.
- Producer write state.
- Producer-to-RR transition or render-graph edge.
- SDK/Streamline read or write state.
- RR-to-post transition.
- Later consumer state.
- Aliasing, transient-resource lifetime, descriptor validity, and resize/recreate path.

Common failure pattern:
A guide buffer is created as shader-resource/read-only, then bound as UAV/render-target by the guide producer without an explicit transition because other nearby guide buffers were transitioned manually. Treat missing transitions for normals, albedo, roughness, motion vectors, specular albedo, and hit distance as hard blockers.

## DLSS-RR SDK/API Requirements

- Use the Streamline DLSS-RR APIs, not SR-only evaluate calls.
- Query Streamline support for DLSS Ray Reconstruction.
- Ensure the required Streamline DLSS-RR plugin can be found at runtime.
- Query DLSS-RR availability, set per-frame Streamline constants, set DLSS-RR options, tag generic depth/motion/UI resources, tag RR-specific resources, evaluate DLSS-RR, and clean up or recreate the feature on mode, preset, resolution, or device changes.
- Configure the unified DLSS denoising mode through Streamline.
- Use DLSS-RR optimal settings for input/output sizing.
- Ignore sharpness, exposure, and auto-exposure parameters for RR.
- Default to RR preset Default unless there is a documented reason to use D or E.
- Do not redistribute NDA/dev DLLs in a public release path.

## DLSS-RR Input Set

Ensure a complete DLSS-RR input set is present at input/render resolution.

Required inputs:

- Noisy ray-traced/path-traced HDR color before tonemap and before final denoising.
- Depth matching the convention used for motion-vector generation.
- Dense primary motion vectors.
- Normals.
- Roughness.
- Diffuse albedo.
- Specular albedo.
- Output target at native/output resolution.
- Either specular/reflection motion vectors, or specular hit distance plus the required WorldToView and ViewToClip matrices.

Validate each RR input:

- Resolution: input/render-res or output-res as required.
- Format.
- Color space.
- Clear value.
- Viewport/scissor coverage.
- Resource state/barriers.
- Whether sky/background pixels are valid, especially specular albedo.
- Whether alpha-tested, transparent, particle, decal, foliage, terrain, skinned, instanced, procedural, and ray-hit content writes valid data or is intentionally handled another way.
- Whether optional inputs are truly omitted or intentionally backed by dummy/placeholder resources. Prefer null/omitted optional resources when the wrapper supports it; document any dummy resource.

## Guide Buffer Anti-Patterns

Catch and report these before judging image quality:

- All-zero normals, diffuse albedo, or specular albedo for valid surface pixels.
- Sky/background pixels left with arbitrary zero material guides without documented handling.
- Roughness, albedo, and normal guides accumulated differently from noisy color.
- Raster G-buffer guides mixed with path-traced color without validating jitter, depth, visibility, and content coverage.
- Specular hit distance present but always zero, always max, stale, or only written for a subset of reflective paths.
- Placeholder/dummy resources passed where the SDK expects meaningful required inputs.

## Path-Traced, Stochastic, and Temporal Inputs

For path-traced, stochastic, or accumulated renderers:

- Trace the exact jitter producer. Distinguish projection jitter, ray/sample jitter, blue-noise offsets, checkerboard offsets, post-process jitter, and any validation-harness jitter. Convert the active jitter to the SDK's expected pixel-space convention and pass it every RR evaluate. Do not leave RR jitter at zero when the color input is jittered.
- Center stochastic pixel jitter around the pixel center unless the renderer has a documented convention. If the renderer samples in `[0,1]` pixel or texture space, convert it to the centered temporal jitter convention before passing it to RR, and keep the motion-vector convention consistent with that choice.
- Log RR evaluate constants at least once per run: jitter, MV scale, reset flag, frame delta, render/output dimensions, depth convention, and whether motion vectors are jittered or unjittered.
- Sanitize frame delta before passing it to RR. Avoid startup, pause, debugger, shader-compile, or loading hitches from appearing as the first temporal frame; use the engine's stable frame delta or a documented fallback when needed.
- Treat RR as a temporal pass. Reset RR history on enable/disable, camera cut, resize, quality/preset change, guide-buffer format change, and render/output resolution change. In validation, let RR settle for multiple evaluated frames after reset before saving a comparison capture.
- For Streamline or backend calls outside the engine's normal render abstraction, explicitly transition or tag resources into the states/layouts expected by the SDK. Inputs must be readable by the SDK, outputs must be writable, and resources should be restored or rebound for subsequent engine passes.
- Make guide-buffer producers coherent. Prefer depth, normal, roughness, albedo, motion, specular albedo, and hit-distance guides from the same path-tracing or path-space layer chain as the noisy color. Do not mix raster, visibility-buffer, or path-traced guide data unless resolutions, jitter, depth convention, camera matrices, and content coverage are explicitly validated.
- Verify depth as a convention pair with motion vectors. Hardware vs linear depth, reversed-Z/depth-inverted flags, infinite far plane, and clip-space matrix handedness must match the create/evaluate parameters and the motion-vector generation path.
- Do not optimize for blur when judging RR against an existing denoiser. RR can preserve more structure and look less smooth than a spatial denoiser; use common-reference metrics, temporal inspection, and artifact checks instead of smoothness alone.

## Accumulated Renderer and RR History

For path tracers or stochastic renderers with built-in accumulation, do not wire RR to the renderer's accumulated or final history buffer as if RR were a post-process denoiser.

Validate that RR receives a coherent current-frame input set every frame:

- Noisy HDR color must be fresh current-frame radiance, usually 1 spp or the renderer's current realtime sample budget.
- Depth, normals, roughness, albedo, motion vectors, specular albedo, and hit distance or specular motion must come from the same current frame and same camera/jitter convention.
- Disable or bypass the renderer's color and guide-buffer accumulation for RR inputs.
- Preserve RR's own temporal history across ordinary camera/object motion; do not reset RR just because the underlying path tracer would reset accumulation.
- Keep scene, acceleration-structure, camera, and previous-frame state updates running every frame while RR is active.
- Treat path-tracer sample index and RR frame/history index as separate concepts.
- When adding a validation mode to an accumulated renderer, separate "static reference accumulation" from "RR realtime input." It is acceptable for the reference/OIDN path to wait for N spp before capture, but RR should evaluate every frame from current-frame noisy input and current-frame guides.

Common symptom:
RR output looks like persistent 1 spp speckle or fails to converge visually, while a spatial denoiser looks smoother after accumulation. This usually means RR is receiving stale, accumulated, reset, or mismatched guide buffers instead of a fresh current-frame signal plus valid temporal state.

Validation:
Run a moving-camera and moving-object sequence. RR should stay denoised during motion, while spatial/offline denoisers may be unavailable until accumulation settles. Log whether RR input accumulation is disabled and whether RR history is preserved.

## Optional Guide Buffers

Add only when needed, but explicitly decide and document each:

- Transparency overlay.
- Color-before-transparency guide.
- Screen-space subsurface scattering guide.
- Depth-of-field guide, only with compatible RR preset.

## Layered or Path-Space Guide Generation

For path-traced renderers that produce layered denoising data, stable planes, path-space decomposition, visibility buffers, or ray-hit records:

- Identify which pass writes noisy HDR radiance, stable/non-noisy radiance, first-hit depth, primary motion vectors, specular hit distance, and per-layer material guides.
- Decide how guide buffers are synthesized from layers: dominant layer, primary layer, throughput-weighted blend, radiance-weighted blend, or renderer-specific policy.
- Ensure the policy handles sky/background, emissive-only pixels, missing secondary layers, perfect specular paths, rough specular paths, transparent/refractive paths, and invalid ray hits.
- Keep guide normals normalized and in the documented space; clamp or repair invalid normals, roughness, and albedos without hiding real missing-data bugs.
- Avoid all-zero diffuse+specular albedo where RR expects a valid material guide. If the renderer uses minimum guide values for sky or stable radiance, document and validate the choice.
- If the RR input color is recomposed from stable radiance plus noisy layer radiance, verify it remains HDR pre-tonemap, pre-exposure conventions are correct, and brightness clamping/local tonemap hacks are documented.
- Expose debug views or captures for each synthesized guide buffer and for any layer weights or selected primary/dominant layer.

## Renderable Inventory

Before implementing RR, inventory every renderable class in the game/application, including:

- Static meshes.
- Rigid moving meshes.
- Skinned/morphed meshes.
- Instanced meshes.
- Particles.
- Billboards.
- Foliage.
- Procedural vertex animation.
- Alpha-tested/blended geometry.
- Decals.
- Impostors.
- Terrain.
- Displacement/tessellation.
- Ray-traced primary hits.
- Ray-traced reflections/refractions.
- Sky/background.
- UI/HUD.

Document whether each class:

- Writes valid primary motion vectors.
- Writes valid specular/reflection motion vectors.
- Writes valid normals/roughness/diffuse albedo/specular albedo.
- Intentionally writes zero vectors.
- Is excluded from RR input.
- Needs a special reset/mask/guide-buffer handling path.

## Motion-Vector Convention

Validate motion-vector convention explicitly:

- Resolution: render-res or output-res.
- Units: UV, NDC, pixels, or normalized.
- Direction/sign: current->previous or previous->current.
- Whether jitter is included.
- Streamline MV scale values used.
- Jitter offset.
- Reset flag.
- Render/output dimensions.
- Frame delta.

Validate whether motion vectors are computed with jittered or unjittered matrices. Inspect the shader/math path and confirm:

- Whether current clip positions include jitter.
- Whether previous clip positions include jitter.
- Whether jitter is already subtracted from the vectors.
- Whether the SDK jitter/motion-vector flags match the actual convention.
- If a wrapper expects normalized motion but the renderer stores pixel-space motion, verify the scale constants and sign with a readback or visualized pan test.

## Motion-Vector Producer Chain

Do not validate or implement motion vectors as only a final buffer. For any RR motion-vector path, identify the complete producer chain that creates the MV input.

Do not mark primary motion vectors complete if they only reproject the current hit position through previous/current camera matrices. That may be sufficient for camera motion, but it does not validate moving rigid objects, skinned meshes, instancing, particles, or moving reflected content. Classify this as camera-motion-only unless previous object/instance/skinning state is proven.

For the primary MV path, document and validate:

- Upstream data consumed by the MV pass, such as depth, primitive IDs, object IDs, visibility buffers, G-buffer data, previous transforms, previous bone matrices, or previous particle/sprite state.
- Shader/pass that writes the final MV buffer.
- Whether the MV pass is raster, compute, ray-traced, visibility-buffer based, depth-reprojection based, or hybrid.
- When current-frame state is produced.
- When previous-frame state is captured.
- When scene/object/camera dirty flags are set and consumed.
- Whether acceleration structures, visibility buffers, primitive IDs, object buffers, instance buffers, and previous transform buffers are updated before MV generation.

If the engine derives MVs from another buffer, such as primitive IDs, depth, visibility IDs, object IDs, or ray-hit data, add diagnostics/readback for both:

- The final MV buffer.
- The immediate upstream source buffer(s).
- The pass or shader stage that produces the upstream source: raster G-buffer, ray generation, closest-hit shader, path-state/stable-plane storage, compute reprojection, or hybrid visibility-buffer pass.

If the final MV buffer is zero, sparse, stale, or suspicious, diagnose in this order:

1. Verify upstream source data exists and is nonzero where motion is expected.
2. Verify the producer pass dispatches/draws and writes the full expected extent.
3. Verify current and previous camera/object/skinning/instance/particle state differ when motion is expected.
4. Verify dirty/change/reset flags are still visible to the normal renderer update path before the renderer consumes them.
5. Verify the output buffer changes after the producer pass.
6. Classify the issue as missing source data, producer pass not running, invalid previous/current state, output write failure, test-harness lifecycle issue, unsupported content class, or real renderer integration bug.

Engine lifecycle requirement:

- Use the engine's normal scene/render update lifecycle for current/previous state tracking.
- Do not manually update transforms, cameras, animation state, acceleration structures, previous matrices, or dirty flags in a way that hides changes from the renderer.

If adding validation scenes or test harness animation:

- One-time setup may initialize transforms/resources explicitly.
- Per-frame animation must happen before the engine update that consumes scene changes.
- Per-frame animation must leave the appropriate dirty/change state visible to the renderer unless the engine's documented API requires otherwise.
- Prove that the validation harness is not bypassing normal previous/current matrix, bone, instance, particle, camera, or acceleration-structure tracking.

Required MV test cases during implementation:

- Static camera + static scene: expect mostly zero motion.
- Moving camera + static scene: expect camera/world/background motion.
- Static camera + moving rigid object: expect object-local motion.
- Moving camera + moving object: expect combined motion, if practical.
- Skinned/animated object: validate previous bones or previous skinned positions, if present.
- Instanced object: validate previous per-instance transforms, if present.
- Particles/sprites/billboards: validate previous screen-space/object-space state or explicit exclusion, if present.
- Reflective moving object or reflected moving object: validate specular/reflection motion handling or specular-hit-distance fallback.

For each MV test case, log:

- Primary MV resource name/format/resolution/nonzero coverage/min/mean/max.
- Upstream source buffer resource name/format/resolution/nonzero coverage/min/mean/max.
- Current/previous transform or state validity.
- Dirty/reset/update flag state before and after renderer update.
- Whether the MV pass used jittered or unjittered matrices.
- Whether jitter was subtracted in the shader or passed separately to Streamline.

## Motion-Vector Implementation Options

If motion vectors are incomplete, implement the smallest correct renderer-specific path:

- Depth reprojection for camera/static-world motion.
- Velocity buffer for geometry/object motion.
- Previous transforms for dynamic objects/instances.
- Previous bone matrices or previous skinned positions for skinned meshes.
- Sprite/particle/2D motion from previous screen-space transform where applicable.
- Explicit mask/reset/exclusion for content that cannot provide valid vectors.

## Specular and Reflection Motion

- If specular motion vectors exist, validate resolution, units, sign, jitter convention, and scale separately from primary motion vectors.
- If specular motion vectors do not exist, provide specular hit distance and the required matrices.
- Verify the matrix order/convention expected by the RR guide.
- If both specular motion vectors and specular hit distance paths exist, choose one active path deliberately and validate the inactive path is not accidentally tagged or half-populated.
- If specular motion is approximated from primary hit position, surface normal, reflection ray, or hit distance, document limitations for curved reflectors, object motion, refraction/transmission, roughness thresholds, and missing hit distances.
- Do not leave a hard-coded or static debug switch controlling the specular-motion strategy in production integration unless it is intentionally surfaced as a debug option.

## Sampling and Noise

- RR inputs should be noisy but valid ray/path traced signals.
- Avoid checkerboard rendering for RR inputs unless explicitly resolved before RR.
- Avoid correlated screen-space dithering and repeated sampling patterns.
- Use high-quality hashes/white-noise-style sampling where practical.
- Use at least 32 jitter phases unless the engine has a documented reason not to.
- Inspect the jitter generator used with RR. R2/Halton/blue-noise/white-noise choices can materially affect RR stability; document any per-pixel micro-jitter or anti-sleep jitter used only for RR.

## Temporal Systems and Cache Interaction

- Identify temporal systems that remain active when RR is selected, including TAA, NRD, ReSTIR DI/GI, lighting reservoirs, exposure history, frame generation, reactive masks, history clamps, and post-process histories.
- Bypass existing denoisers while RR is active unless a documented renderer-specific reason requires otherwise.
- Reset temporal caches when switching RR on/off, changing RR preset/quality, resizing, changing render/output resolution, changing camera cut/reset state, or changing required guide-buffer format.
- If a temporal lighting or sampling system is not tuned for RR input noise, either disable it while RR is active or validate it with comparable captures and temporal inspection.
- Ensure frame generation consumes the post-RR/pre-UI or project-appropriate color path and that UI/HUD tagging/composition does not feed UI into RR.

## Logging

Log enough information to prove:

- Requested RR mode.
- Actual active RR mode.
- Integration surface: Streamline, engine plugin, or local Streamline wrapper.
- Mode selector state and fallback destination: disabled/noisy, TAA, DLSS-SR/DLAA, existing denoiser, or RR.
- RR support-query result.
- RR create/evaluate result.
- Internal/render resolution.
- RR input resolution.
- RR output/native resolution.
- Final window/swapchain/present resolution.
- Active RR preset.
- Every required RR input buffer name/format/resolution.
- Primary MV producer-chain summary.
- Upstream MV source buffer names/formats/resolutions/nonzero coverage.
- Current/previous camera/object/skinning/instance state validity for MV generation.
- Dirty/change/reset flag state relevant to MV generation.
- Whether the validation harness uses the normal renderer update lifecycle.
- Whether the existing denoiser was bypassed or still used.
- Which temporal lighting/cache systems were disabled, reset, or left active with validation.
- Tonemap/UI/HUD composition order.
- Whether RR input color/guides are fresh current-frame buffers or accumulated renderer history, and how RR history is kept separate from renderer accumulation.

## Post-RR Validation

- Verify the post-RR pipeline writes the full output extent.
- Check viewport, scissor, tonemap dispatch, and later fullscreen passes at native/output resolution.
- Verify UI/HUD is composed after RR unless the engine intentionally does otherwise.
- Verify no UI ghosting or upscale blur is introduced by accidentally feeding UI into RR.
