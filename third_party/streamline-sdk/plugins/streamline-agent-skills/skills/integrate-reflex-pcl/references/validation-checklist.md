# Reflex/PCL Validation Checklist

Use this checklist to validate and fix a Streamline Reflex/PCL integration end to end. Do not stop at reporting bugs. If validation finds a reasonably local issue, implement the fix, rebuild, rerun the relevant validation, and report before/after evidence.

## Contents

- Scope
- Grounded Understanding
- Initialization Validation
- Options Validation
- Debug Instrumentation
- Frame Token Validation
- Reflex Sleep Validation
- PCL Marker Validation
- Runtime Matrix
- Verification Tooling
- Unsupported and Runtime-Missing Validation
- Build and Static Validation
- Final Validation Report Template

## Scope

Validate Reflex/PCL only. Do not add or validate super-resolution, frame generation, ray reconstruction, or unrelated graphics features except to ensure unrelated existing features are not broken by Reflex/PCL changes.

Use the Streamline SDK as the source of truth:

- SDK repository: https://github.com/NVIDIA-RTX/Streamline
- `docs/ProgrammingGuideReflex.md`
- `include/sl_reflex.h`
- `include/sl_pcl.h`
- `include/sl_core_api.h`
- `include/sl_core_types.h`
- `source/plugins/sl.reflex/reflexEntry.cpp`
- `source/plugins/sl.reflex/reflex.json`
- Any existing Streamline, Reflex, PCL, latency marker, or NVAPI Reflex wrapper code in the target game

## Grounded Understanding

Inspect:

- Existing render loop.
- Input loop.
- Simulation/update loop.
- Present path.
- Swapchain resize/restart path.
- Frame limiter.
- VSync handling, including the actual present/swapchain mode used at runtime.
- Settings ownership and late reapplication paths: config files, graphics menus, startup scripts, console variables, command-line overrides, platform overlays, and any UI widgets that can overwrite requested VSync/frame-limiter/Reflex state after launch.
- Streamline/Reflex/PCL/NVAPI latency code.
- Whether rendering, command submission, or present can happen on another thread/task.
- Exact sleep point relative to prior present completion and next input sampling.
- Build systems and runtime copy/package steps.

## Initialization Validation

Confirm:

- Streamline initializes before Vulkan/D3D/DXGI device and swapchain use where required.
- Reflex and PCL are both requested/loaded.
- The graphics device is registered with Streamline through the existing device setup path.
- `slReflexGetState` is called after device setup.
- Reflex UI is controlled only by `ReflexState::lowLatencyAvailable`.
- No manual GPU vendor/device gating is used outside SDK state.
- Missing Streamline runtime DLLs are handled without crashing.
- PCL markers remain active where PCL is available.

If any item is missing or ordered incorrectly, patch it and rebuild.

## Options Validation

Validate:

- User modes exist: Off, On / Low Latency, and On + Boost / Low Latency With Boost.
- Modes map to `ReflexMode::eOff`, `ReflexMode::eLowLatency`, and `ReflexMode::eLowLatencyWithBoost`.
- `slReflexSetOptions` is called at least once during startup/settings initialization, including Off.
- Runtime option changes call `slReflexSetOptions` again.
- `frameLimitUs` is wired if the engine exposes a Reflex frame limiter.
- Invalid latency flash hotkeys/options are not exposed to users.
- Unsupported Reflex UI states are disabled using SDK state, not manual vendor checks.

Patch and retest stale options, startup-only options, incorrect mode mapping, or unsupported UI gating.

## Debug Instrumentation

Add or validate debug instrumentation:

- Gate noisy logs behind an existing or new debug cvar/setting.
- Log Streamline runtime path and missing runtime DLLs.
- Log Reflex/PCL load status.
- Log Reflex availability, selected mode, `frameLimitUs`, and `latencyReportAvailable`.
- Log every frame token in debug mode.
- Log sleep result and count per frame.
- Log every PCL marker name, frame token/frame ID, and result.
- Log marker order regressions.
- Log mixed/stale frame IDs.
- Keep production logs targeted and avoid noisy default output.

Add or validate console/status commands if the engine has a console:

- `reflex status`
- `reflex mode off|on|boost`
- `reflex framelimit <microseconds>`
- `reflex debug 0|1`

## Frame Token Validation

Confirm:

- Exactly one fresh Streamline frame token is created/retrieved per engine frame.
- The same token is used for `slReflexSleep` and all PCL markers for that frame.
- Stale tokens are not reused.
- The frame token is not overwritten or cleared before render submit and present markers have used it.
- If render submit/present can run on another thread, the token cannot be overwritten before those markers are emitted.

If tokens are mixed between simulation, render, or present, patch synchronization or token ownership and retest.

## Reflex Sleep Validation

Confirm:

- `slReflexSleep` is called exactly once per rendered frame.
- It is called even when Reflex mode is Off, as long as the Reflex feature is loaded.
- It uses the current frame token.
- It happens after previous frame render/present work is submitted or joined as appropriate for the engine.
- It happens before input sampling and simulation for the new frame.
- Sleep count per frame is logged in debug mode.

Patch and retest if sleep is missing in Off mode, called more than once, called before previous present completes, or called after input sampling.

## PCL Marker Validation

Expected order per presented frame:

1. `InputSample`
2. `SimulationStart`
3. `SimulationEnd`
4. `RenderSubmitStart`
5. `RenderSubmitEnd`
6. `PresentStart`
7. `PresentEnd`

Required placement:

- `InputSample` wraps the actual input polling/sampling point.
- `SimulationStart`/`SimulationEnd` wrap game update, command accumulation, server/client simulation, and readback from server as applicable.
- `RenderSubmitStart`/`RenderSubmitEnd` wrap command recording/submission for the frame.
- `PresentStart` is immediately before the real present call.
- `PresentEnd` is immediately after present returns.
- `TriggerFlash`/`PCLatencyPing` are wired only where the game supports latency flash/PCL ping.
- Markers are emitted in every Reflex mode, including Off.
- Markers use the same frame token as `slReflexSleep` for the frame.

Patch and retest approximate, out-of-order, missing, duplicated, or mixed-token markers.

## Runtime Matrix

Run and verify:

- Reflex Off.
- Reflex On.
- Reflex On + Boost.
- VSync on/off. Confirm the real present path changed, not just the requested setting. On D3D, log or inspect sync interval, present flags, tearing allowance, swapchain frame-latency waitable object/max latency where used, and fullscreen/windowed state. On Vulkan, log or inspect the selected present mode and acquire/present pacing.
- Engine frame limiter on/off.
- Reflex `frameLimitUs` on/off if exposed.
- Window resize.
- Fullscreen/windowed transitions.
- Alt-tab/device-loss path if practical.
- Threaded renderer path enabled/disabled if present.
- Unsupported GPU path if available.
- Missing Streamline runtime path.
- Missing `sl.reflex.dll` path.
- Missing `sl.pcl.dll` path.

For each failure, capture logs, identify root cause, patch the integration, rerun the smallest failing case, and rerun affected matrix entries.

## Verification Tooling

Use Reflex/PCL verification tooling from the SDK docs when available.

Confirm:

- App Called Sleep = 1 in Off, On, and On + Boost.
- Marker timestamps are nonzero.
- Frame IDs are fresh and not mixed.
- Marker order is correct.
- `slReflexSleep` uses the same frame token as PCL markers for the frame.
- `ReflexState` latency report availability and sane reports on supported NVIDIA hardware.
- PCL markers remain active where PCL is available.
- Reflex On does not cause more than roughly 4% FPS regression in normal gameplay.
- On + Boost may change power/perf behavior but remains stable.

If the tool reports failures, patch and rerun until clean or document a real blocker.

## Unsupported and Runtime-Missing Validation

Confirm:

- The game runs when Streamline runtime is missing.
- The game runs when `sl.interposer.dll`, `sl.common.dll`, `sl.reflex.dll`, or `sl.pcl.dll` is missing.
- The game runs on unsupported GPUs.
- Reflex UI is disabled when `lowLatencyAvailable` is false.
- Reflex/PCL feature functions are not called through unavailable function pointers.
- PCL markers remain active where PCL is available, even if low-latency Reflex is unavailable.
- Logs identify missing DLLs and exact expected paths.

Patch fallback behavior if unsupported paths fail.

## Build and Static Validation

- Run the configured build command.
- Fix compile, warning-as-error, link, shader/package, and runtime-load issues caused by Reflex/PCL changes.
- Run `git diff --check`.
- Preserve unrelated dirty worktree changes.

## Final Validation Report Template

Report:

- Summary: PASS / FAIL.
- Files changed.
- Build command.
- Build result.
- Run commands for Off, On, and On + Boost.
- Streamline runtime DLL status.
- Reflex/PCL verification tool results.
- App Called Sleep result for every mode.
- Marker order/frame-token evidence.
- PCL marker timestamp result.
- `ReflexState` latency report result.
- Runtime toggle result.
- VSync/frame limiter/resize/fullscreen/threaded-renderer result, including actual present mode evidence and any mismatch between requested and effective state.
- Unsupported GPU/runtime-missing result.
- Performance regression result.
- Remaining risks.
- Exact files/functions fixed.
- Exact files/functions still needing fixes, if any remain.
