---
name: integrate-dlss-rr
description: Integrate and validate NVIDIA DLSS Ray Reconstruction (DLSS-RR/DLSSD) in a game engine using NVIDIA Streamline or project Streamline wrappers, SDK documentation, project-specific RR gotchas, and bundled integration/validation checklists. Use when an AI coding agent is asked to add, debug, verify, or code-review DLSS-RR, ray/path-traced denoising, DLSSD inputs, Streamline RR resource tagging, path-space/layered guide-buffer generation, G-buffer generation, motion-vector producer chains, specular-hit-distance or specular-motion-vector paths, denoiser/temporal-cache ordering, or RR runtime toggle support in an engine.
---

# DLSS-RR Integration

## Skill Source

Treat `skills/integrate-dlss-rr/` as the shared skill source for Codex and Claude Code.

- Keep `SKILL.md` and `references/` portable Markdown with relative links.
- Keep frontmatter and links portable across Codex and Claude; do not add runtime-specific metadata, slash-command assumptions, or workstation-specific install paths.

## Example Requests

Use this skill for requests like:

- "Integrate DLSS Ray Reconstruction into this path-traced renderer."
- "Validate DLSS-RR guide buffers and fresh noisy input placement."
- "Debug RR artifacts caused by motion vectors or specular hit distance."
- "Check that RR runs before tonemap and replaces the existing denoiser."
- "Review whether DLSS-RR and DLSS-SR are mutually exclusive in this pipeline."

## Core Workflow

Use the target NVIDIA Streamline SDK package as the source of truth:

- Streamline headers and documentation inside that package, especially the DLSS-RR guide material
- If the project already uses an engine-owned Streamline wrapper, use that abstraction where practical and verify that it maps to the DLSS-RR feature, RR options, required constants, resource tags, runtime plugins, and evaluate call.
- Project-specific RR checklist, bug notes, or gotchas supplied by the user, repository, or host environment
- If no project-specific notes exist, do not stop and ask the user to write them. Derive a temporary engine profile from the repository and logs, then use that profile as the local plan for the current task.

Integration quality levels:

Classify every RR integration before finalizing:

- MVP: RR runs, consumes fresh current-frame noisy HDR input, receives all required guide buffers, runs before tonemap/post, falls back cleanly, and passes basic resource-state, jitter, motion-vector, resize, and toggle validation.
- Production candidate: MVP plus validated object/skinning/instance motion vectors, robust guide coverage for major renderable classes, deterministic captures, temporal stability checks, and known runtime fallback paths.
- Reference quality: Production candidate plus layered/path-space guide synthesis, validated specular/reflection motion strategy, broad scene coverage, objective comparisons against references, and documented edge-case handling.

Do not present an MVP integration as reference quality. Explicitly report known limitations and likely follow-up PRs.

Production quality bar:

- Make the smallest coherent renderer-style change that would survive outside a fixture.
- Do not add comments, string literals, dummy variables, marker names, or validator-specific tokens only to satisfy a check. Validation evidence must come from real integration logic: buffer setup, resource tags, frame ordering, previous/current transforms, motion-vector generation, and RR evaluate placement.
- Prefer named data paths, helpers, and state that match the target renderer's architecture instead of one-off test-case branches.
- Keep the implementation concise; avoid large switch blocks, repeated evidence plumbing, or report-only code that does not affect the RR data path.
- If working in a synthetic fixture, implement the real structure the fixture represents and state that runtime GPU validation remains outside the fixture.

RR motion-vector and guide specifics:

- Moving object with static camera: derive object MVs from valid current and previous object transforms or animation state; keep camera/background motion stable except for jitter compensation.
- Moving camera plus moving object: combine camera reprojection with object transform delta for the same frame identity; do not replace this with a generic combined-motion flag.
- Instanced geometry: preserve per-instance IDs and current/previous instance transforms; do not use a single global previous transform for all instances.
- Skinned or animated geometry: use current/previous pose, bone, morph, or animation state in the MV producer path; rigid-object MV validation is not enough.
- Particles, sprites, and billboards: handle current/previous particle position, velocity or lifetime, spawned/dead particles, and billboard orientation policy; document exclusions as limitations instead of passing them silently.
- Reflective/specular content: provide specular hit distance and a reflection/specular motion strategy with valid matrix/ray-hit handling; account for perfect specular, rough specular, sky/background, and invalid hit cases where applicable.
- MVP hard gates: require fresh current-frame noisy input, complete guide buffers, RR before tonemap/post/existing denoisers, no DLSS-SR second pass after RR, and separate requested/active/fallback state.

Avoid these anti-patterns:

- Adding strings such as `mv.instanced`, `rr.hard_gates`, or `specularHitDistance` without corresponding code paths.
- Putting validation-only comments or `recordCheck` calls in renderer code as the main evidence.
- Passing a motion-vector case using only final-buffer existence when the upstream producer chain is missing.
- Claiming MVP, production, reference, or runtime validation when the build, run, capture, or readback was not performed.

Before editing code:

1. Read the SDK docs relevant to DLSS-RR/DLSSD and the target renderer/API.
2. Read the Streamline or engine-wrapper documentation/source if that is the project's active DLSS integration layer.
3. Read any project-specific RR checklist, bug notes, or gotchas supplied by the user, repository, or host environment and treat them as a checklist.
4. Read [references/integration-checklist.md](references/integration-checklist.md).
5. Inspect the engine structure instead of assuming it. Find the renderer, ray/path-tracing path, denoiser path, temporal lighting/cache systems, post-processing chain, swapchain/output path, command-line/config system, logging system, and resource lifetime/recreation model.
6. Build a short engine profile before implementation. Record the build entrypoint, executable, graphics API/backend, DLSS integration surface, existing denoisers, scene/capture mechanism, relevant sample scenes, and where deterministic command-line or config toggles can be added. Treat this as a living checklist and update it as discoveries are proven or disproven.

During implementation:

- Treat DLSS-RR as its own integration path. Do not assume DLSS-SR exists, and do not run DLSS-SR as a second pass after RR.
- If the project has an AA/SR/denoiser mode selector, make RR a mutually exclusive mode alongside disabled/noisy, TAA, DLSS-SR/DLAA, and existing denoiser modes where practical.
- Add a runtime flag, config option, command-line option, menu option, or debug toggle that matches the project.
- Keep requested RR state separate from actual active RR state.
- Ensure unsupported backend/GPU, missing runtime DLL/shared object, support-query failure, feature creation failure, required-buffer failure, and evaluate failure all fall back cleanly and log why RR is inactive.
- Place RR before tonemap and before most post-processing, as close to the start of post-processing as practical.
- If an existing denoiser exists, add runtime modes for noisy/no denoiser, existing denoiser, and DLSS-RR where practical. RR must consume noisy ray/path-traced HDR input before the existing denoiser unless the renderer requires a documented alternative.
- Feed RR the complete required DLSSD input set at the correct resolutions.
- Treat the MVP hard gates in [references/integration-checklist.md](references/integration-checklist.md) as blockers for a working first integration. If a hard gate cannot be satisfied, report RR as incomplete or intentionally limited instead of calling it done.
- Keep render/input resolution, RR output/native resolution, and final present resolution explicit. After RR, verify later fullscreen/composition passes use output resolution.
- Validate motion vectors as a producer chain, not just a final buffer. Read back or otherwise inspect immediate upstream MV source data when practical.
- If the renderer uses path-space decomposition, stable planes, visibility buffers, ray-hit records, or layered denoising guides, document how noisy color, normals, roughness, albedos, primary MVs, specular MVs, and hit distances are synthesized from those layers.
- For path-traced, stochastic, or accumulated renderers, validate jitter convention, frame delta, temporal settle behavior, resource states, and coherent guide-buffer producers before judging RR image quality.
- For accumulated renderers, keep RR input sampling and RR temporal history separate from renderer accumulation: feed RR fresh current-frame noisy color and matching current-frame guides, while preserving RR history across ordinary camera/object motion.
- If the renderer lacks a ready-made validation path, add the smallest deterministic harness that fits the engine: fixed scene load, fixed camera path, noisy/existing-denoiser/RR/reference modes where practical, logged sample counts, and flat image/video outputs with mode-specific filenames.
- Identify temporal systems that interact with RR, such as TAA, NRD, ReSTIR/RTXDI, lighting caches, exposure, frame generation, and history clamps. Reset, bypass, or explicitly validate them when RR toggles or falls back.
- Disable, reject, or cleanly fall back from dynamic resolution while RR is active because RR does not support DRS.
- Rebind engine global graphics state after Streamline evaluation if backend calls may disturb it.

After implementation:

1. Read [references/validation-checklist.md](references/validation-checklist.md).
2. Build the game/application.
3. Run with RR disabled and with RR requested/enabled.
4. Capture deterministic frames and verify RR inputs, producer chains, pixels, resolutions, composition order, resize/toggle behavior, fallback states, and MAE/MSE against a common reference where available.
5. Run at least one static scene and one moving camera or moving object sequence. In static scenes, RR and the existing denoiser should be visually comparable after settle; during motion, RR should remain stable while spatial/offline denoisers may be unavailable, noisy, or delayed until accumulation settles.
6. Perform a code-review pass over the integration. Call out anything unusual, fragile, incomplete, or counterintuitive before finalizing.

## Final Response

Report the following:

- Files changed.
- Integration quality level: MVP, production candidate, or reference quality.
- MVP hard-gate pass/fail table.
- Build command.
- Run command with RR disabled.
- Run command with DLSS-RR enabled.
- Runtime flag/config/menu used.
- Printed render/internal, RR input, RR output/native, and present resolutions.
- RR requested state vs actual active state.
- RR input buffer table with format/resolution/status.
- Resource-state/render-graph audit summary for every RR input and output.
- Motion-vector convention summary.
- Motion-vector producer-chain summary.
- Specular/reflection motion handling summary.
- Whether RR input color/guides are fresh current-frame buffers or accumulated renderer history, and how RR history is kept separate from renderer accumulation.
- Image comparison matrix results, including RR/noisy/existing-denoiser/reference pair definitions where available.
- MAE/MSE values and scale used when a common reference is available.
- Regional pixel findings.
- Runtime fallback validation findings.
- Resize/toggle findings.
- Guide-buffer known limitations, including sky/background, transparent content, particles, skinned meshes, decals, and specular/reflection handling.
- Follow-up PR list, separated into correctness fixes, quality improvements, validation improvements, and optional reference-quality work.
- Any incomplete areas, intentional exclusions, fragile assumptions, or counterintuitive code paths.
