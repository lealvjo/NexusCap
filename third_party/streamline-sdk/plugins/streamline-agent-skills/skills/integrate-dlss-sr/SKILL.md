---
name: integrate-dlss-sr
description: Integrate and validate NVIDIA DLSS Super Resolution (DLSS-SR) in a game engine using NVIDIA Streamline, SDK documentation, project-specific SR gotchas, and bundled integration/validation checklists. Use when an AI coding agent is asked to add, debug, verify, or code-review DLSS-SR, upscaling, motion-vector, jitter, render-resolution, composition-order, or DLSS runtime toggle support in an engine.
---

# DLSS-SR Integration

## Skill Source

Treat `skills/integrate-dlss-sr/` as the shared skill source for Codex and Claude Code.

- Keep `SKILL.md` and `references/` portable Markdown with relative links.
- Keep frontmatter and links portable across Codex and Claude; do not add runtime-specific metadata, slash-command assumptions, or workstation-specific install paths.

## Core Workflow

Use the target NVIDIA Streamline SDK package as the source of truth:

- Streamline headers and documentation inside that package
- Project-specific integration notes supplied by the user, repository, or host environment

Production quality bar:

- Make the smallest coherent renderer-style change that would survive outside a fixture.
- Do not add comments, string literals, dummy variables, marker names, or validator-specific tokens only to satisfy a check. Validation evidence must come from real SR integration logic: input buffer binding, resolution plumbing, jitter/reset/frame timing, motion-vector generation, feature state, fallback paths, and UI composition order.
- Prefer named data paths, helpers, and state that match the target renderer's architecture instead of one-off test-case branches.
- Keep the implementation concise, but do not reduce the change to marker-only evidence or a report-only stub.
- If working in a synthetic fixture, implement the real structure the fixture represents and state that runtime GPU validation remains outside the fixture.

SR input and resolution specifics:

- Track internal render size, DLSS output/native size, and final present size as separate values. Validate that DLSS input resources use render resolution and post-DLSS fullscreen/UI passes use output or present resolution as appropriate.
- Bind HDR scene color before tonemap and UI when available, depth, motion vectors, output color, jitter offset, reset, frame delta, MV scale, and exposure or auto-exposure state.
- Keep requested DLSS state separate from actual active DLSS state and preserve a clear fallback reason.
- Compose HUD/UI after DLSS-SR unless the engine has a documented UI path that supplies UI masks or alpha through a supported integration.

SR motion-vector specifics:

- Static camera/static scene: primary MVs should be mostly zero except expected jitter compensation; depth, color, and metadata must remain current-frame and valid.
- Camera pan: use current and previous camera/view-projection state, correct MV sign/units/scale, jitter convention, frame delta, and reset-on-cut behavior.
- Moving object with static camera: derive object MVs from valid current and previous object transforms while keeping static background MVs stable.
- Skinned or animated geometry: use current/previous pose, bone, morph, or animation state in the MV producer path; rigid-object MV validation is not enough.
- If renderable classes lack valid MVs, mask, reset, exclude, or document them as limitations instead of silently passing validation.

Avoid these anti-patterns:

- Adding strings such as `mv.camera_pan`, `mv.skinned_animated`, or `sr.resolution` without corresponding code paths.
- Putting validation-only comments or `recordCheck` calls in renderer code as the main evidence.
- Passing a motion-vector case using only final-buffer existence when the upstream producer chain is missing.
- Claiming runtime/GPU validation when only the static fixture validator was run.

Before editing code:

1. Read the SDK docs relevant to the target renderer/API.
2. Read any project-specific SR checklist, bug notes, or gotchas supplied by the user, repository, or host environment and treat them as a checklist.
3. Read [references/integration-checklist.md](references/integration-checklist.md).
4. Inspect the engine structure instead of assuming it. Find the renderer, swapchain/output path, post-processing chain, command-line/config system, logging system, and resource lifetime/recreation model.

During implementation:

- Add a runtime flag, config option, command-line option, or debug toggle that matches the project.
- Keep requested DLSS state separate from actual active DLSS state.
- Ensure DLSS failure, unsupported backend, missing DLL, feature creation failure, and evaluate failure all fall back cleanly and log why DLSS is inactive.
- Log render/internal resolution, DLSS/output resolution, final window/swapchain/present resolution, jitter, reset, frame delta, MV convention, exposure mode, and composition order.
- Feed DLSS a complete input set: HDR scene color before tonemap/UI when the engine has HDR, depth, and motion vectors.
- If the engine is SDR-only, continue with SDR as appropriate and explicitly report that HDR scene color was not available.
- Verify post-DLSS fullscreen passes write the full native output extent.
- Rebind engine global graphics state after Streamline evaluation if backend calls may disturb it.

For motion vectors:

- Inventory every renderable class before implementation.
- Confirm whether current/previous clip positions include jitter, whether jitter is subtracted from vectors, and whether the SDK `JitteredMotionVectors` or equivalent flag matches the actual convention.
- Validate MV resolution, units, sign, jitter inclusion, Streamline MV scale values, jitter offset, reset flag, render/output dimensions, and frame delta plumbing.
- If motion vectors are incomplete, implement the smallest renderer-appropriate path that covers the rendered content, or explicitly exclude/mask/reset classes that cannot produce valid vectors.

After implementation:

1. Read [references/validation-checklist.md](references/validation-checklist.md).
2. Build the game.
3. Run with DLSS disabled and enabled.
4. Capture deterministic frames and verify pixels, resolutions, composition order, resize/toggle behavior, and fallback states.
5. Perform a code-review pass over the integration. Call out anything unusual, fragile, or counterintuitive before finalizing.

## Final Response

Report the following:

- Files changed.
- Build command.
- Run command with DLSS disabled.
- Run command with DLSS-SR enabled.
- Printed render/internal, DLSS/output, and present resolutions.
- MSE results for DLSS-SR vs native, and bilinear vs native if captured.
- Regional pixel inspection findings.
- Motion-vector validation findings.
- Runtime fallback validation findings.
- Any incomplete areas, intentional exclusions, or fragile assumptions.
