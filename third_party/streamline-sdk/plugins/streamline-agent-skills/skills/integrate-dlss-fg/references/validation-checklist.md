# DLSS-FG Validation Checklist

Use this checklist after wiring DLSS Frame Generation. Do not stop after API integration. If validation finds a reasonably local issue, fix it, rebuild, rerun the relevant validation, and report before/after evidence.

## Contents

- Evidence Levels
- Grounded Profile
- Real-Scene Validation Setup
- Build and Static Validation
- Runtime Dependency Validation
- Production and Development Mode Validation
- Streamline Setup Validation
- DLSS-G Support and State Validation
- Options and Toggle Validation
- Non-Game-Frame Validation
- Active Real-Scene Frame Validation
- Required Input Validation
- Motion-Vector Producer-Chain Validation
- Present and Swapchain Validation
- Optional Quality Validation
- Visual Capture Requirement
- Capture Scenarios
- Logs to Check
- Failure Handling
- Final Validation Report Template

## Evidence Levels

Report each level as PASS, FAIL, PARTIAL, or NOT RUN:

1. Build/static evidence.
2. Runtime dependency/fallback evidence.
3. Streamline/SDK functional evidence from logs.
4. Real-scene frame evidence from repeated active frames.
5. Visual artifact evidence from screenshot/video/manual/tool inspection.
6. Performance/pacing evidence.

Do not claim full PASS unless the relevant evidence exists. If only logs prove activation, report functional integration as PASS but visual validation as PARTIAL or NOT RUN.

## Grounded Profile

Before validating, record:

- Graphics backend and queue model.
- Abstraction boundaries.
- Streamline proxy/manual setup path.
- Project/application identifier used by Streamline.
- Swapchain create/recreate/present path.
- Vulkan acquire/present or D3D12 backbuffer index path.
- Common constants path.
- Required tag path for depth and motion vectors.
- Optional UI/HUD/distortion tags.
- Settings/toggle path, including UI/config/console/command-line ownership and any late reapplication of saved graphics settings.
- Menu/loading/pause/non-game-frame behavior.
- Motion-vector producer chain.
- Runtime DLL/package path.
- Production/development mode switch and mode-specific runtime/config path.
- Available real scenes/test maps/sample apps.
- Available screenshot/video/backbuffer capture paths.

## Real-Scene Validation Setup

- Identify an existing representative scene/map/sample.
- Launch it through command line, startup script, test harness, replay, sample app, or editor automation.
- Ensure the scene has visible geometry, non-empty depth, non-empty motion vectors, camera motion, and the normal present path.
- Include at least one moving object when available.
- Include UI/HUD if the engine has one.
- Enable DLSS-G debug logs for this run.
- Capture a screenshot/video if possible.
- If capture fails, preserve logs and explicitly mark visual validation as blocked.
- Do not count an empty editor/menu viewport as enabled real-scene validation.
- Remove temporary validation files/scripts afterward unless they are intentionally committed.

## Build and Static Validation

- Configure with the local SDK/runtime path.
- Build the application.
- Verify required runtime DLL copy/install outputs.
- Run `git diff --check`.
- If build/runtime copy fails, patch and rebuild.
- Preserve unrelated dirty worktree changes.

## Runtime Dependency Validation

- Run with runtime DLLs present.
- Run with DLSS-G/Streamline runtime DLLs missing or renamed if practical.
- Verify missing-runtime fallback logs and no crash.
- Run with `sl.dlss_g.dll` missing if practical.
- Run with the required Streamline DLSS-G plugin missing if practical.
- Verify required Vulkan runtime dependencies such as `NvLowLatencyVk.dll` are present when needed.
- Confirm unsupported or missing dependency paths disable FG cleanly.

## Production and Development Mode Validation

When the integration exposes a production/development mode switch:

- Build or launch production mode and confirm production/non-watermarked runtimes are selected.
- Confirm production mode disables Streamline console/debug text and does not show development debug visualization or watermark.
- Build or launch development mode and confirm development/non-production runtimes are selected when available.
- Confirm development mode enables SDK console or verbose logs when supported and leaves Streamline debug text/visualization enabled.
- Confirm `sl.dlss_g.json` or equivalent debug visualization config is copied or referenced only in development/diagnostic mode.
- Confirm logs report selected mode, runtime path, debug text state, console/log level, and missing mode-specific files.
- Confirm development DLLs/configs are not included in production packaging.

If development runtimes are unavailable, report this validation as NOT RUN rather than weakening production behavior.

## Streamline Setup Validation

- Confirm Streamline initializes before swapchain/present use where required.
- Confirm a valid project/application identifier is used.
- Confirm no Streamline identifier validation error.
- If using Vulkan/DXGI proxy path, confirm proxy dispatch owns device/swapchain setup and explicit `slSetVulkanInfo` or `slSetD3DDevice` is not incorrectly called.
- If using manual path, confirm `slSetVulkanInfo` or `slSetD3DDevice` is called immediately after device creation and before feature function/support paths.
- Confirm no "plugins already initialized before device setup" error.
- Patch ordering/path errors and retest.

## DLSS-G Support and State Validation

Confirm DLSS-G feature support query succeeds or fails cleanly.

Confirm feature loaded state is logged.

Confirm `slDLSSGGetState` reports:

- Status.
- `numFramesToGenerateMax`.
- `numFramesActuallyPresented`.
- `minWidthOrHeight`.
- VSync support.
- Dynamic MFG support.
- Estimated VRAM if intentionally queried.

Also confirm:

- Requested generated-frame count is clamped to max.
- Unsupported GPU, driver, HWS, or backend paths disable FG with a clear reason.

## Options and Toggle Validation

Test:

- Off.
- On.
- Auto if exposed.
- Dynamic only if exposed and supported.
- Generated-frame count changes.
- Generated-frame count clamp above SDK max.
- Runtime toggle Off to On to Off during gameplay if an automation, console, or UI path exists.

Confirm:

- Requested mode/count, effective mode/count, and actual present-active state are distinct and logged.
- `slDLSSGSetOptions` is not spammed every frame.
- There is no repeated `slDLSSGSetOptions` warning during startup, menu, or normal frames.
- Unsupported modes are hidden, disabled, or cleanly mapped to fallback.

Patch unsupported or partially-active modes and retest.

## Non-Game-Frame Validation

- Run with FG requested while starting in menus/loading screens.
- Confirm DLSS-G is configured or available but actual present-active mode stays off/deferred until valid frame inputs exist.
- Confirm no missing-common-constants warning during normal menu/non-game rendering.
- Confirm menus, loading, and pause do not deadlock or present stale generated frames.
- Patch if FG activates without constants/tags.

## Active Real-Scene Frame Validation

For every frame where FG is active:

- Common constants are set before present.
- Depth is tagged for the same frame.
- Motion vectors are tagged for the same frame.
- Resources are valid until present.
- Constants, tags, app frame identity, and presented frame align.
- Actual active state is reported only when the current frame is fully prepared.

For the real-scene enabled run, report:

- Total rendered frames observed.
- Count of frames with common constants set.
- Count of frames with depth tags.
- Count of frames with motion-vector tags.
- First/last frame ID and token.
- Whether frame IDs/tokens are fresh and monotonic.
- Number of `slDLSSGSetOptions` calls.
- Whether `slDLSSGSetOptions` was called only on real transitions.
- Actual present-active mode.
- `numFramesActuallyPresented` from `DLSSGState`.
- Any skipped/deferred/fallback frame count and reason.

## Required Input Validation

Validate:

- Depth extent, format, state/layout, and lifetime.
- Motion-vector extent, format, state/layout, and lifetime.
- Motion-vector scale.
- Motion-vector sign.
- Pixel vs normalized motion-vector units.
- Jittered vs non-jittered convention.
- Reset/camera-cut handling.
- Dynamic resolution dimensions.
- Current and previous camera matrices.
- Depth inverted flag.
- Camera motion included flag.
- Stale or mixed frame identity detection.

Patch local convention/metadata issues where possible and retest.

## Motion-Vector Producer-Chain Validation

Validate or document coverage for:

- Camera motion.
- Terrain/static objects.
- Rigid moving meshes.
- Instanced objects.
- Skinned/animated objects.
- Particles.
- Alpha-tested objects.
- Translucent objects.
- Decals.
- Animated materials.
- Foliage.
- Velocity dilation or invalid-value policy.

Do not stop at inspecting the final motion-vector texture. Trace the producer chain: upstream state, previous/current transforms, dirty flags, pass execution, barriers/layouts, and output stats. Classify missing or stale motion as camera-motion-only support, missing producer data, producer pass not running, invalid previous/current state, output write failure, test-harness lifecycle issue, unsupported content class, or renderer integration bug.

## Present and Swapchain Validation

- Confirm Streamline owns/intercepts the intended main swapchain only.
- Confirm proxy backbuffers/internal swapchain behavior matches SDK expectations.
- Confirm resize/recreate disables or defers FG safely.
- Confirm fullscreen/windowed transition is safe.
- Confirm alt-tab/device-loss path if practical.
- For Vulkan, verify acquire/present semaphore correctness and image-index ownership.
- For D3D12, verify `GetCurrentBackBufferIndex` behavior where required.
- Confirm no deadlock, leak, stale state, or present error.
- Confirm actual present/swapchain state matches the intended validation mode. On D3D, log or inspect sync interval, present flags, tearing allowance, backbuffer index behavior, and fullscreen/windowed state. On Vulkan, log or inspect selected present mode, acquire/present pacing, semaphore usage, and image-index ownership.
- Patch local present/swapchain lifecycle issues and retest.

## Optional Quality Validation

Validate or document:

- HUDless color tag if implemented.
- UI alpha or UI color+alpha tag if implemented.
- Whether `Blend(HUDless, UI) == FinalColor` when UI recomposition is used.
- Reticle, subtitles, cursor, platform overlays, damage indicators, and menus.
- Distortion/refraction/scope/heat-haze handling or documented limitation.
- HDR format support and fallback.
- VSync/VRR policy using `DLSSGState` VSync support, including effective present mode evidence and any mismatch between requested UI/config state and actual runtime state.
- Pacing: generated/real frame cadence, dropped generated frames, and actual presents per app frame.

## Visual Capture Requirement

Try, in order:

1. Engine screenshot/backbuffer capture.
2. Engine video/capture command if available.
3. OS/window capture.
4. External capture/analysis tool if installed and practical.

If none work, do not claim visual validation PASS. Report: "Functional integration proven by logs; visual artifact validation not proven."

## Capture Scenarios

Run or document why each could not be run:

- Static scene with no UI.
- Moving camera through dense geometry.
- Moving animated/skinned object.
- Fast camera pan with foreground edges.
- UI-heavy scene with HUD/subtitles/reticle.
- Particles/transparency/foliage.
- Distortion/refraction/scope/heat-haze if present.
- Menu/loading/pause transition.
- Resize/fullscreen/alt-tab.
- FG toggle Off to On to Off during gameplay.
- Camera cut/teleport/scene load.

## Logs to Check

Explicitly check:

- Project ID accepted; no invalid identifier error.
- No plugins-initialized-before-device error.
- Selected production/development runtime mode, debug text state, console/log level, and mode-specific runtime/config path.
- DLSS-G supported/loaded state.
- DLSSGState availability/status/max frames.
- Requested/effective/active mode.
- Startup/deferred-off behavior.
- `slDLSSGSetOptions` result.
- No repeated `slDLSSGSetOptions` warning.
- Common constants set result.
- Depth/motion-vector tag result.
- Frame ID/frame token for constants/tags/present.
- No stale or mixed frame IDs.
- No missing-common-constants warning on frames where FG is active or correctly deferred.
- Generated frames actually presented during gameplay when active.
- No present/acquire errors.
- No marker/order regressions if Reflex/PCL logging is also present.

## Failure Handling

For each failure:

- Capture relevant log lines.
- Identify root cause.
- Patch the smallest local integration issue.
- Rebuild.
- Rerun the smallest failing case.
- Rerun affected matrix entries.
- Provide before/after evidence.

## Final Validation Report Template

Include:

- Summary table:
  - Build/static: PASS / FAIL
  - Runtime optional fallback: PASS / FAIL
  - Streamline setup: PASS / FAIL
  - DLSS-G functional activation: PASS / FAIL
  - Real-scene frame tagging: PASS / FAIL
  - Visual artifact validation: PASS / FAIL / PARTIAL / NOT RUN
  - Performance/pacing validation: PASS / FAIL / PARTIAL / NOT RUN
  - Overall: PASS / FAIL / PARTIAL
- Files/build tested.
- Build command.
- Build result.
- Disabled run command.
- Enabled/requested run command.
- Real-scene run command/map/script used.
- Integration quality level: MVP, production candidate, or reference quality.
- Runtime dependency result.
- Production/development runtime mode and debug-visualization result.
- Project/application identifier result.
- Proxy/manual device setup result.
- `DLSSGState` table:
  - Status.
  - Max generated frames.
  - Requested generated frames.
  - Effective generated frames.
  - Actual presented frames.
  - VSync support.
  - Dynamic MFG support.
- Requested/effective/actual-active mode table.
- Present/swapchain audit, including actual present mode evidence and requested-versus-effective settings mismatches.
- Required input tag table: depth and motion vectors.
- Optional input tag table: HUDless, UI alpha/color, backbuffer extent, distortion.
- Common constants/frame-identity audit.
- Resource-state and lifetime audit.
- Motion-vector convention and producer-chain summary.
- Non-game-frame/menu/loading fallback result.
- Runtime toggle result.
- Resize/fullscreen/alt-tab result.
- Pacing/latency findings.
- Visual artifact matrix by scenario.
- Screenshot/video/capture artifacts produced, if any.
- Before/after evidence for any fixes made.
- Known limitations and follow-up PRs split into correctness, quality, validation, and optional reference-quality work.
