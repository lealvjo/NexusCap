# Reflex/PCL Integration Checklist

Use this checklist before and during code edits.

## Contents

- Required References
- Engine Profile
- Initialization and Feature Loading
- Reflex Options
- Sleep Placement
- Frame Tokens
- PCL Markers
- NVAPI Reflex Migration
- Runtime Matrix
- Build and Packaging
- Logging and Debug Commands
- Acceptance Criteria
- Final Build Report Template

## Required References

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

Read the SDK docs and inspect existing engine latency code before adding new abstractions.

## Engine Profile

Inspect and record:

- Render loop.
- Input polling/sampling loop.
- Simulation/update loop.
- Command recording and command submission path.
- Present path and exact real present call.
- Swapchain resize/restart path.
- Fullscreen/windowed transition path.
- Alt-tab/device-loss path if present.
- VSync handling, including the actual present/swapchain parameters used at runtime.
- UI/config/console/command-line ownership for VSync, frame limiters, fullscreen state, and Reflex settings, including code that can reapply saved settings after startup overrides.
- Engine frame limiter and Reflex frame limiter options.
- Threading/task model.
- Whether rendering, command submission, or present can happen on another thread/task.
- Existing Streamline/device setup path.
- Existing Reflex/PCL/NVAPI latency marker code.
- Console/config/menu/runtime option path.
- Build systems and runtime DLL copy/package steps.

Identify the exact sleep point relative to previous frame render/present completion and next input sampling.

## Initialization and Feature Loading

- Initialize Streamline early enough for the graphics API path, before D3D/DXGI/Vulkan device and swapchain use where required.
- Enable/load both Reflex and PCL.
- Set the graphics device through the existing Streamline/device setup path.
- Query Reflex state with `slReflexGetState` after device setup.
- Use `ReflexState::lowLatencyAvailable` to enable or disable Reflex UI.
- Do not manually gate Reflex on GPU vendor/device checks outside SDK state.
- PCL markers should remain active where PCL is available, even if low-latency Reflex is unavailable.
- Treat missing runtime DLLs, unsupported GPU, unavailable feature functions, or SDK failures as fallback conditions with clear logs.

## Reflex Options

Add user-facing modes:

- Off.
- On / Low Latency.
- On + Boost / Low Latency With Boost.

Map modes:

- Off -> `ReflexMode::eOff`.
- On -> `ReflexMode::eLowLatency`.
- On + Boost -> `ReflexMode::eLowLatencyWithBoost`.

Requirements:

- Support `frameLimitUs` if the engine exposes a Reflex frame limiter.
- Call `slReflexSetOptions` at least once during startup/settings initialization, even if Reflex is Off.
- Call `slReflexSetOptions` again after runtime Reflex option changes.
- Keep selected mode, effective mode, and runtime dependency/fallback state explicit in logs/UI.
- Do not expose invalid latency flash hotkeys/options unless the game has a real latency flash/ping path.

## Sleep Placement

Call `slReflexSleep` once per rendered frame:

- Call it in all Reflex modes, including Off, as long as Reflex is loaded.
- Use the current frame token.
- Place it after previous frame work has been submitted/presented or joined as appropriate for the engine.
- Place it before input sampling and simulation for the next frame.
- Do not call it twice for one frame.
- Do not skip it in Off mode when Reflex is loaded.
- Log sleep result and count per frame in debug mode.

If render submit or present is asynchronous, prove the sleep point is still ordered correctly relative to the previous frame's work and the next input sampling.

## Frame Tokens

Create or retrieve exactly one fresh Streamline frame token per engine frame.

Use the same token for:

- `slReflexSleep`.
- `InputSample`.
- `SimulationStart`.
- `SimulationEnd`.
- `RenderSubmitStart`.
- `RenderSubmitEnd`.
- `PresentStart`.
- `PresentEnd`.
- Optional `TriggerFlash` or `PCLatencyPing` only if those paths are real.

Rules:

- Do not reuse stale tokens.
- Do not overwrite or clear the token before all render submit and present markers for that frame have been emitted.
- If render submit/present can run on another thread, ensure token ownership/lifetime is explicit and thread-safe.
- Detect and log mixed/stale frame IDs.
- Record frame ID/frame token in debug logs.

## PCL Markers

Required marker order per presented frame:

1. `InputSample`
2. `SimulationStart`
3. `SimulationEnd`
4. `RenderSubmitStart`
5. `RenderSubmitEnd`
6. `PresentStart`
7. `PresentEnd`

Placement:

- `InputSample` wraps the actual input polling/sampling point.
- `SimulationStart`/`SimulationEnd` wrap game update, command accumulation, server/client simulation, and readback from server where applicable.
- `RenderSubmitStart`/`RenderSubmitEnd` wrap command recording/submission for the frame.
- `PresentStart` is immediately before the real present call.
- `PresentEnd` is immediately after present returns.
- `TriggerFlash`/`PCLatencyPing` are wired only where the game supports latency flash/PCL ping.
- Markers are emitted in every Reflex mode, including Off.
- Markers use the same frame token as `slReflexSleep` for the frame.

Log marker names, frame IDs/tokens, results, order regressions, duplicates, missing markers, and mixed/stale frame IDs behind a debug setting.

## NVAPI Reflex Migration

If NVAPI Reflex exists, replace or bridge cleanly:

- `NvAPI_D3D_SetSleepMode` -> `slReflexSetOptions`.
- `NvAPI_D3D_Sleep` -> `slReflexSleep`.
- `NvAPI_D3D_SetLatencyMarker` -> PCL/Streamline marker path.

Do not leave two independent Reflex systems active. Preserve existing user settings and fallback behavior where practical.

## Runtime Matrix

Preserve behavior across:

- Reflex Off.
- Reflex On.
- Reflex On + Boost.
- VSync on/off.
- Engine frame limiter on/off.
- Reflex `frameLimitUs` on/off if exposed.
- Window resize.
- Fullscreen/windowed transitions.
- Swapchain resize/restart.
- Alt-tab/device loss if practical.
- Threaded renderer path enabled/disabled if present.
- Unsupported GPU path if available.
- Missing Streamline runtime path.
- Missing `sl.interposer.dll`.
- Missing `sl.common.dll`.
- Missing `sl.reflex.dll`.
- Missing `sl.pcl.dll`.

For every failure, capture logs, identify root cause, patch the integration, rerun the smallest failing case, and rerun affected matrix entries.

## Build and Packaging

Update every relevant build system:

- Source lists.
- Include directories.
- Compile definitions.
- C++ language mode if needed.
- PCH settings/exceptions if needed.
- Linker settings.
- Runtime DLL copy/install/package steps.

Distinguish Streamline SDK headers/source from redistributable runtime DLLs.

Runtime DLLs needed for this scope:

- `sl.interposer.dll`
- `sl.common.dll`
- `sl.reflex.dll`
- `sl.pcl.dll`

If runtime DLLs are missing, keep the integration runtime-optional and log clear instructions/paths.

Avoid broad SDK helper headers if they pull in optional graphics API types unsupported by the project headers.

Run the configured build, fix compile/link/runtime-load issues, and run `git diff --check`. Preserve unrelated dirty worktree changes.

## Logging and Debug Commands

Add targeted logging/debug output for:

- Streamline runtime path.
- Missing Streamline runtime DLL paths.
- Reflex/PCL feature load status.
- Reflex availability.
- Selected Reflex mode.
- Effective Reflex mode, if a runtime dependency requires one.
- `frameLimitUs`.
- Frame token/frame ID.
- Whether `slReflexSleep` was called.
- Sleep result.
- Sleep count per frame.
- PCL marker names.
- Marker frame IDs/frame tokens.
- Marker results.
- Marker order regressions.
- Mixed/stale frame IDs.
- `ReflexState` latency report availability.

Keep noisy per-frame logs behind a debug cvar or setting.

Add status commands if the engine has a console:

- `reflex status`
- `reflex mode off|on|boost`
- `reflex framelimit <microseconds>`
- `reflex debug 0|1`

## Acceptance Criteria

- The game builds successfully.
- The game runs when Streamline runtime is missing.
- The game runs on unsupported GPUs with Reflex UI disabled.
- Reflex options can be changed at runtime.
- `slReflexSetOptions` is called at startup/settings init, including Off.
- `slReflexSetOptions` is called again after runtime Reflex option changes.
- `slReflexSleep` is called once per frame in all Reflex modes when Reflex is loaded.
- `slReflexSleep` uses the current frame token.
- PCL markers are emitted in the correct order with matching frame tokens.
- Marker order is `InputSample -> SimulationStart -> SimulationEnd -> RenderSubmitStart -> RenderSubmitEnd -> PresentStart -> PresentEnd`.
- Marker order regressions are logged.
- Mixed/stale frame token use is detected and logged.
- `ReflexState` reports sane availability and latency data on supported NVIDIA hardware.
- Actual present state matches the intended validation mode: for example D3D `Present` sync interval/flags, DXGI tearing state, Vulkan present mode, fullscreen/windowed state, VRR/VSync policy, and active engine/driver frame limiter state are logged or otherwise verified after UI/config has settled.
- PCL markers remain active where PCL is available, even if low-latency Reflex is unavailable.
- Unsupported GPU/runtime-missing paths do not crash.
- VSync, frame limiter, swapchain resize, fullscreen/windowed transitions, and render-thread paths remain stable.

## Final Build Report Template

Report:

- Summary: PASS / FAIL.
- Files changed.
- Build command used.
- Build result.
- Run command(s).
- Runtime dependency notes.
- Streamline DLLs found/missing.
- Static validation result.
- Runtime validation result.
- Reflex/PCL verification tool result, if run.
- App Called Sleep result, if verified.
- Marker/frame-token evidence.
- Marker order evidence.
- `ReflexState` latency report result.
- Runtime toggle result.
- VSync/frame limiter/resize/fullscreen/threaded-renderer result.
- Unsupported GPU/runtime-missing result.
- Performance regression result, if measured.
- Remaining risks.
- Exact files/functions fixed.
- Exact files/functions still needing fixes.
