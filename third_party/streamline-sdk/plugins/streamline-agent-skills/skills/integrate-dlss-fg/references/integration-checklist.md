# DLSS-FG Integration Checklist

Use this checklist before and during code edits.

## Contents

- Required References
- Engine Profile
- Integration Surface
- Streamline Setup
- Runtime Dependencies
- Production and Development Mode
- Project Identifier
- Swapchain and Present Path
- Common Constants and Required Tags
- Optional Tags
- DLSS-G State and Options
- Non-Game Frames and Fallback
- Motion-Vector Producer Chain
- Validation Harness
- Build and Packaging
- Logging
- Acceptance Criteria
- Final Build Report Template

## Required References

Use the NVIDIA Streamline SDK and local SDK package as the source of truth:

- `docs/ProgrammingGuideDLSS_G.md`
- `include/sl_dlss_g.h`
- `include/sl.h`
- `include/sl_core_api.h`
- `include/sl_core_types.h`
- `include/sl_helpers_vk.h` when Vulkan is used
- `source/plugins/sl.dlss_g/*` when present
- `source/core/sl.api` and `source/core/sl.interposer` Vulkan/DXGI code when present
- Runtime logs from release SDK DLLs when plugin source is not included

Read the DLSS-G guide and relevant headers before editing code. Do not assume DLSS-G behaves like SR, RR, or a generic post-process evaluate pass.

## Engine Profile

Build a grounded profile before implementation:

- Graphics backend: D3D12 or Vulkan, abstraction boundaries, queue model, swapchain ownership, and main present path.
- Existing Streamline or engine-owned Streamline wrapper and project/application identifier path.
- Swapchain path: create, resize, recreate, fullscreen/windowed transition, acquire, present, and shutdown.
- Present hook or proxy path: Streamline proxy dispatch vs explicit manual device registration.
- Backbuffer indexing: D3D12 `GetCurrentBackBufferIndex` or Vulkan image-index/acquire flow.
- Common constants path: matrices, previous matrices, jitter, reset, depth convention, motion-vector scale, frame identity, and frame delta.
- Required resource tag path: depth and motion vectors with lifetimes valid until present.
- Optional resource tag path: HUDless color, UI alpha or color+alpha, backbuffer extent, and distortion field.
- Motion-vector producer chain: camera, terrain/static objects, instancing, skinned/animated objects, particles, transparency, foliage, decals, and animated materials when present.
- Runtime settings path: menu, config, console, command line, automation flags.
- Settings ownership and late reapplication paths for VSync, frame limiters, fullscreen/windowed mode, FG mode/count, and present-active state. Identify UI widgets or config loaders that can overwrite launch overrides after startup.
- Non-game-frame paths: menus, loading, pause, cutscenes, resize, camera cuts, editor viewports, and scene transitions.
- Runtime packaging path for Streamline runtime and plugin DLLs.
- Available real scenes, test maps, samples, startup scripts, replay systems, or command-line scene loading.
- Available screenshot, video, or backbuffer capture paths.

Prefer deriving engine-specific steps from checked-in scripts, CI files, build files, launch configs, sample apps, editor command-line handlers, and existing validation tools.

## Integration Surface

- Use the existing Streamline or engine-owned Streamline abstraction where practical.
- Do not bypass a project wrapper unless it cannot expose DLSS-G support/state, options, constants, resource tags, swapchain/present setup, runtime dependency status, and fallback logging.
- Keep DLSS-G state separate from DLSS-SR, DLSS-RR, Reflex, latency markers, and any upscaling or denoising state.
- Do not add or validate SR, RR, or unrelated features except to ensure existing features are not broken.
- If the engine has a graphics-feature mode selector, add DLSS-G as a frame-generation option rather than a post-process AA/upscaling/denoising mode.

## Streamline Setup

- Initialize Streamline early enough for the selected device/proxy path.
- If using Streamline Vulkan proxy dispatch, obtain and use Streamline's `vkGetInstanceProcAddr` before Vulkan instance, device, and swapchain creation. Do not also call `slSetVulkanInfo` as if the app were manual-only unless the SDK path explicitly requires it.
- If not using Streamline Vulkan/DXGI proxies, call `slSetVulkanInfo` or `slSetD3DDevice` immediately after graphics device creation and before feature functions or support queries that initialize plugins.
- Do not call DLSS-G feature functions before the Streamline device/proxy path is valid.
- Confirm there is no "plugins already initialized before device setup" ordering error.
- Log the selected setup path: proxy dispatch, manual device registration, or engine wrapper.

## Runtime Dependencies

Request/load DLSS-G through Streamline and handle dependencies as optional runtime components:

- `sl.interposer.dll`
- `sl.common.dll`
- `sl.dlss_g.dll`
- `NvLowLatencyVk.dll` on Vulkan when required by the SDK/runtime

Keep SDK headers/source distinct from redistributable runtime DLLs. Runtime DLLs must be copied next to the executable only when configured and present. Missing DLLs must not prevent the application from running with FG disabled.

Log:

- Streamline runtime path.
- Selected production/development runtime mode, debug text state, console/log level, and mode-specific runtime/config path.
- Loaded DLL/runtime dependency status.
- Missing DLL paths.
- Whether the runtime copy/install/package step produced the expected files.
- Clear instructions or paths when runtime DLLs are missing.

## Production and Development Mode

Add or validate a build-time, launch-config, or packaging switch for Streamline runtime mode when practical.

Production mode:

- Use production/non-watermarked Streamline and DLSS-G runtime binaries.
- Disable Streamline console output and SDK debug text/visualization.
- Do not copy development JSON/config files unless explicitly required for diagnostics.
- Treat this as the default mode for shipping and automated release builds.

Development mode:

- Use development/non-production Streamline and DLSS-G runtime binaries when available.
- Enable Streamline console output or verbose logs when supported.
- Leave SDK debug text/visualization enabled; do not set `PreferenceFlags::eDisableDebugText` in Streamline integrations that rely on the overlay.
- Copy or point at `sl.dlss_g.json` or equivalent debug visualization config only when intentionally present.
- Log that development DLLs/configs must not ship.

Always log the selected mode, runtime path, debug text/console state, and whether the expected mode-specific binaries/configs were found.

## Project Identifier

- Use a valid NVIDIA project or application identifier required by the Streamline integration.
- Keep Streamline project and application identifiers consistent.
- Do not invent an arbitrary string such as a game name when a registered identifier is required.
- Log the project/application identifier path and whether Streamline accepted it.
- Treat identifier validation failure as clean FG fallback.

## Swapchain and Present Path

DLSS-G depends on present-path ownership and current-frame tagging.

- Configure DLSS-G cleanly when swapchain/resources are created or recreated.
- Do not report FG active until a frame with valid constants and tags is actually presented.
- Preserve the engine's normal swapchain create, resize, recreate, fullscreen/windowed, acquire, present, and shutdown behavior.
- For Vulkan, preserve acquire/present semaphore correctness and image-index ownership.
- For D3D12, preserve backbuffer index correctness.
- Confirm Streamline owns/intercepts only the intended main swapchain.
- Disable or defer FG during resize, swapchain recreation, fullscreen/windowed transition, alt-tab/device loss, loading screens, menus, pause, non-game-frame rendering, and camera cuts unless complete valid inputs are provided.
- Recreate or revalidate options and resources after swapchain, resolution, display-mode, or format changes.
- Log present/acquire errors and fallback decisions.
- Verify actual present/swapchain state for the tested mode, not only requested settings. On D3D, log or inspect sync interval, present flags, tearing allowance, backbuffer index behavior, and fullscreen/windowed state. On Vulkan, log or inspect selected present mode, acquire/present pacing, and image-index ownership.

## Common Constants and Required Tags

For every frame where FG is active:

- Set common constants for that same app frame.
- Tag depth for that same app frame.
- Tag motion vectors for that same app frame.
- Keep tagged resources valid until present.
- Ensure present consumes the same frame identity as constants and tags.

Required tag table fields:

- Resource name.
- Format.
- Extent.
- State/layout at tag time.
- Producer pass.
- Lifetime until present.
- Frame ID or token.
- Success/failure result.

Validate:

- Depth extent, format, state/layout, depth convention, and lifetime.
- Motion-vector extent, format, state/layout, lifetime, scale, sign, units, jitter convention, and reset/camera-cut behavior.
- Dynamic resolution or render/output dimensions when applicable.
- Current and previous camera matrices.
- Depth inverted flag.
- Camera motion included flag.
- Stale or mixed frame identity detection.

Treat missing constants, missing tags, stale tags, stale frame identity, mixed frame identity, or failed tag calls as fallback conditions with explicit logs.

## Optional Tags

Tag optional production-quality resources where available:

- HUDless color.
- UI alpha.
- UI color+alpha.
- Backbuffer extent for subrects.
- Distortion field for strong refraction, lens effects, scopes, heat haze, or similar effects.

For each optional tag, record:

- Available / implemented / omitted.
- Resource name, format, extent, state/layout, and lifetime.
- Why omitted, if not implemented.
- Any expected visual limitation from omission.

When UI recomposition is used, validate or document whether `Blend(HUDless, UI) == FinalColor` for representative HUD, reticle, subtitles, cursor, platform overlays, damage indicators, and menus.

## DLSS-G State and Options

Query DLSS-G support and `DLSSGState` after valid device/proxy setup.

Log:

- Supported/loaded state.
- Status.
- `numFramesToGenerateMax`.
- `numFramesActuallyPresented`.
- `minWidthOrHeight`.
- VSync support.
- Dynamic MFG support.
- Estimated VRAM only when intentionally queried.

Add runtime options:

- Off.
- On.
- Auto only when supported and appropriate.
- Dynamic only when supported and appropriate.
- Generated-frame count or multiplier when exposed.

State rules:

- Keep requested mode/count separate from effective configured mode/count and actual present-active state.
- Clamp requested generated-frame count to `DLSSGState::numFramesToGenerateMax`.
- Keep unsupported modes hidden, disabled, or mapped to a logged fallback.
- Avoid redundant `slDLSSGSetOptions` calls. Call it on settings/swapchain changes and real state transitions such as deferred-off to active tagged frame, or active to disabled.
- Do not spam `slDLSSGSetOptions` every frame.
- Log requested mode/count, effective mode/count, actual present-active mode, clamping, option-apply result, and option-apply reason.

## Non-Game Frames and Fallback

DLSS-G should not activate on frames that lack current-frame constants and required tags.

Fallback or defer during:

- Startup before tagged gameplay frames.
- Main menu, loading screen, pause screen, editor-only viewport, and cutscenes when complete inputs are missing.
- Camera cuts, teleports, scene loads, and reset frames.
- Resize, swapchain recreation, fullscreen/windowed transition, alt-tab, or device loss.
- Unsupported GPU, driver, HWS, backend, VSync policy, runtime dependency, support query, feature creation, present hook, or option apply failure.

Do not log missing-common-constants warnings during normal menu/non-game rendering when FG is correctly deferred. Log a concise deferred-off reason instead.

## Motion-Vector Producer Chain

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

Do not validate motion vectors only as a final buffer. Trace the producer chain: upstream state, previous/current transforms, pass/shader, resource barriers, and update lifecycle. If only camera motion is covered, report that limitation.

## Validation Harness

Add or use a non-invasive validation launch path that can run a real scene without manual editor interaction.

Prefer existing:

- Command-line scene loading.
- Startup script hooks.
- Test maps.
- Sample apps.
- Replay systems.
- Automation flags.

If none exist, add the smallest local validation hook needed, gated behind a command-line flag or debug-only path.

The validation scene must exercise:

- Non-empty depth.
- Non-empty motion vectors.
- Camera motion.
- At least one moving object when available.
- Normal present path.
- UI/HUD when the engine has one.

Do not count an empty editor/menu viewport as the enabled smoke test. Temporary validation scripts/files may be used, but remove them afterward unless they are intentionally part of the integration.

## Build and Packaging

Update every relevant build system:

- Source lists.
- Include directories.
- Compile definitions.
- C++ language mode if needed.
- Linker settings.
- Runtime DLL copy/install/package steps.
- Production/development runtime-mode switch, selected runtime directory, and any mode-specific debug config copy rules.

Requirements:

- Keep integration runtime-optional if Streamline/DLSS-G DLLs are missing.
- Run the configured build.
- Fix compile, warning-as-error, link, shader/package, and runtime-load issues.
- Run `git diff --check`.
- Preserve unrelated dirty worktree changes.

## Logging

Add targeted logs for:

- Streamline runtime path.
- Selected production/development runtime mode, debug text state, console/log level, and mode-specific runtime/config path.
- Loaded DLL/runtime dependency status.
- Missing DLL paths.
- Project/application identifier used by Streamline.
- Proxy/manual device setup path.
- DLSS-G support/load result.
- `DLSSGState` summary.
- Requested mode/count.
- Effective mode/count.
- Actual present-active mode.
- Startup/deferred-off state.
- Generated-frame count clamping.
- Common constants set result.
- Depth tag result.
- Motion-vector tag result.
- Frame ID/frame token used for constants/tags/present.
- Missing constants/tags fallback.
- Stale/mixed frame identity fallback.
- `slDLSSGSetOptions` result.
- Present/acquire errors.
- Resize/toggle fallback.

Keep per-frame logs behind a debug cvar or setting.

In debug mode, also log or summarize:

- Frames where common constants were set.
- Frames where depth was tagged.
- Frames where motion vectors were tagged.
- Frames where DLSS-G options were applied.
- Frames where actual present-active mode was On, Auto, or Dynamic.
- Skipped/deferred frames and reasons.
- Stale or mixed frame identity count.

## Acceptance Criteria

Functional integration PASS requires:

- The game builds successfully.
- The game runs when DLSS-G/Streamline runtime DLLs are missing.
- The game runs on unsupported GPUs/drivers with FG disabled and clear logs.
- DLSS-G support and state are queried after valid Streamline device/proxy setup.
- Runtime options exist and can be changed.
- Requested FG mode/count is distinct from effective mode/count and actual present-active state.
- Generated-frame count is clamped to SDK max.
- FG does not activate on untagged menu/loading/non-game frames.
- No missing-common-constants warning during normal menu/non-game rendering.
- No repeated `slDLSSGSetOptions` warning from startup/deferred mode handling.
- Required depth and motion-vector tags are emitted for active real-scene frames.
- Common constants and tags correspond to the frame being presented.
- Resize/swapchain recreation/toggle paths fall back cleanly.

Visual validation PASS requires captured image/video or human/tool inspection confirming no obvious corruption/artifacts in representative motion scenarios.

Performance validation PASS requires generated-frame behavior and FPS/pacing measured with a tool or engine telemetry.

If visual capture or performance tooling is unavailable, report those as `PARTIAL` or `NOT RUN` rather than claiming full PASS.

## Final Build Report Template

Report:

- Summary table:
  - Build/static: PASS / FAIL
  - Runtime optional fallback: PASS / FAIL
  - Streamline setup: PASS / FAIL
  - DLSS-G functional activation: PASS / FAIL
  - Real-scene frame tagging: PASS / FAIL
  - Visual artifact validation: PASS / FAIL / PARTIAL / NOT RUN
  - Performance/pacing validation: PASS / FAIL / PARTIAL / NOT RUN
  - Overall: PASS / FAIL / PARTIAL
- Files changed.
- Integration quality level: MVP, production candidate, or reference quality.
- Runtime options/config/menu/console commands added.
- Requested-versus-effective settings mismatches for VSync, frame limiter, fullscreen/windowed mode, FG mode/count, and actual present-active state.
- Build command.
- Build result.
- Run command with FG disabled.
- Run command with FG requested/enabled.
- Real-scene validation command/map/script used.
- Runtime dependency notes.
- Production/development runtime mode and debug-visualization result.
- Requested mode/count vs effective mode/count vs actual present-active mode.
- `DLSSGState` summary.
- Project/application identifier result.
- Proxy/manual device setup result.
- Swapchain/present integration summary.
- Required input tag table.
- Optional input tag table.
- Frame-token/frame-identity evidence.
- Motion-vector producer-chain summary.
- Fallback behavior summary.
- Static validation result.
- Runtime smoke result.
- Visual capture result.
- Performance/pacing result if measured.
- Known limitations and follow-up PRs split into correctness, quality, validation, and optional reference-quality work.
