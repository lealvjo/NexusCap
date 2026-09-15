# DLSS-SR Validation Checklist

Use this checklist after wiring DLSS-SR. Do not stop after API integration.

## Contents

- Captures
- Image Metrics
- Motion-Vector Validation
- Runtime State Validation
- Full-Output Write Validation
- Composition-Order Validation
- Resize and Toggle Validation
- Final Report Template

## Captures

Capture deterministic frames for:

- DLSS disabled/native.
- DLSS-SR enabled.
- Bilinear upscale baseline, if practical.

Print these resolutions for each run:

- Render/internal resolution.
- DLSS/output resolution.
- Final window/swapchain/present resolution.

Confirm:

- Without DLSS-SR, render resolution equals output resolution.
- With DLSS-SR, internal render resolution is lower and final output resolution is native.

## Image Metrics

Compute:

- DLSS-SR vs native MSE.
- Bilinear vs native MSE, if practical.

Always perform regional pixel inspection:

- Bottom rows.
- Right columns.
- Corners.
- Center crop.
- Max-error pixels.
- Black/zero pixels where native is nonzero.

If MSE is high or error is localized, diagnose and fix the cause. Check for:

- Unwritten edge pixels.
- Viewport/scissor mismatch.
- Render-target size mismatch.
- Compute dispatch sizing bugs.
- Color-space mismatch.
- Tonemapping/exposure order mismatch.
- Temporal jitter/history issues.
- UI/HUD mismatch.
- CPU/software rendering fallback.

## Motion-Vector Validation

Validate motion vectors with:

- Static camera and static scene.
- Fast camera pan.
- Moving object with static camera.
- Skinned or animated object when present.

Inspect ghosting/trails and confirm MV scale/sign are correct.

## Runtime State Validation

Test:

- Requested DLSS on but unsupported backend.
- Missing required Streamline DLSS-SR plugin.
- Streamline feature-creation failure, if practical.
- Streamline evaluation failure, if practical.

Confirm actual active mode falls back cleanly and logs why.

## Full-Output Write Validation

- Inspect final row/column after DLSS.
- Inspect final row/column after tonemap.
- Verify no later pass uses internal-resolution viewport/scissor/dispatch dimensions against output-resolution targets.

## Composition-Order Validation

- Confirm HUD/text/menu overlays are drawn after DLSS.
- Confirm UI does not ghost or upscale blur unless intentionally rendered before DLSS.
- Log the composition order in the run output.

## Resize and Toggle Validation

- Enable DLSS, resize the window, and verify feature/resource recreation.
- Toggle native -> DLSS -> native repeatedly.
- Change DLSS preset/quality mode and verify resources recreate or resize correctly.

## Final Report Template

Include:

- Files changed.
- Build command.
- Run command with DLSS disabled.
- Run command with DLSS-SR enabled.
- Printed resolutions.
- MSE results.
- Regional pixel findings.
- Motion-vector validation findings.
- Runtime fallback validation findings.
- Resize/toggle findings.
- Code-review findings, especially anything unusual, fragile, or counterintuitive.
