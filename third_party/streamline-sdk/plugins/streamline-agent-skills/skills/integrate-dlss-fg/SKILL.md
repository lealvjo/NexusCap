---
name: integrate-dlss-fg
description: Integrate and validate NVIDIA DLSS Frame Generation (DLSS-FG/DLSS-G) in a game engine using NVIDIA Streamline or engine-owned Streamline wrappers, SDK documentation, present/swapchain integration, current-frame constants and resource tags, runtime fallback, and bundled integration/validation checklists. Use when an AI coding agent is asked to add, debug, verify, or code-review DLSS-G, frame generation, generated-frame count or multiplier support, Streamline proxy/manual device setup, DLSSGState handling, present-path ownership, menu/loading fallback, real-scene frame tagging, or frame pacing validation in an engine.
---

# DLSS-FG Integration

## Skill Source

Treat `skills/integrate-dlss-fg/` as the shared skill source for Codex and Claude Code.

- Keep `SKILL.md` and `references/` portable Markdown with relative links.
- Keep frontmatter and links portable across Codex and Claude; do not add runtime-specific metadata, slash-command assumptions, or workstation-specific install paths.

## Example Requests

Use this skill for requests like:

- "Add DLSS Frame Generation to this Streamline renderer."
- "Validate our DLSS-G present path and swapchain handling."
- "Debug why DLSS Frame Generation is requested but not active."
- "Check whether depth, motion vectors, HUDless color, and UI tags are correct for DLSS-G."
- "Review DLSS-G fallback behavior when Streamline or its DLSS-G plugin is missing."

## Core Workflow

Use the NVIDIA Streamline SDK and local SDK package as the source of truth:

If the local Streamline SDK release package is not included in the repository or workspace, stop and ask the user to add it and tell the agent when it is available so the skill can proceed.

- `docs/ProgrammingGuideDLSS_G.md`
- `include/sl_dlss_g.h`
- `include/sl.h`
- `include/sl_core_api.h`
- `include/sl_core_types.h`
- `include/sl_helpers_vk.h` when Vulkan is used
- `source/plugins/sl.dlss_g/*` when plugin source is present
- `source/core/sl.api` and `source/core/sl.interposer` Vulkan/DXGI code when present
- Runtime logs from release SDK DLLs when plugin source is not included

DLSS-G is not a normal post-process evaluate pass. Treat it as present-path integration that depends on Streamline setup, current-frame common constants, required resource tags, swapchain/present ownership, and clean fallback.

Integration quality levels:

- MVP: DLSS-G is runtime-optional, support/state are queried after valid Streamline device or proxy setup, requested/effective/actual-active state is separated, required depth and motion-vector tags are emitted for active real-scene frames, and unsupported or missing-runtime paths fall back cleanly.
- Production candidate: MVP plus validated swapchain resize/toggle paths, menu/loading deferral, real-scene automation, repeated active-frame evidence, capture-based visual inspection, and basic FPS/pacing telemetry.
- Reference quality: Production candidate plus broad scene coverage, optional HUD/UI/distortion tagging where applicable, automated visual and pacing analysis, runtime dependency matrix coverage, and documented edge-case behavior for non-game frames, camera cuts, alt-tab, fullscreen, and device loss.

Do not present an MVP integration as reference quality. Report missing visual or performance evidence as `PARTIAL` or `NOT RUN` instead of claiming full pass.

Before editing code:

1. Read the DLSS-G SDK docs and headers relevant to the target renderer/API.
2. Read the Streamline or engine-wrapper source if that is the project's active DLSS integration layer.
3. Read [references/integration-checklist.md](references/integration-checklist.md).
4. Inspect the engine structure instead of assuming it. Find the graphics backend, swapchain/present path, Streamline wrapper, common constants path, required resource tag path, command-line/config/menu system, logging system, runtime packaging, real-scene launch path, and capture path.
5. Build a short engine profile before implementation. Record backend, queue model, swapchain ownership, proxy/manual setup path, backbuffer indexing, constants/tags, motion-vector producer chain, runtime settings, non-game-frame paths, runtime DLL path, real scenes, and screenshot/video capture options.

During implementation:

- Scope the change to DLSS Frame Generation / DLSS-G. Do not add or validate SR, RR, or unrelated graphics features except to avoid breaking existing features.
- Use the existing Streamline or engine-owned Streamline abstraction where practical. Do not bypass it unless no usable abstraction exists.
- Initialize Streamline early enough for the selected proxy/manual path. For Vulkan proxy dispatch, obtain and use Streamline's `vkGetInstanceProcAddr` before Vulkan instance/device/swapchain creation. For manual device registration, call `slSetVulkanInfo` or `slSetD3DDevice` immediately after device creation and before feature/support work.
- Use the valid NVIDIA project or application identifier required by the Streamline integration; do not invent an arbitrary game-name string.
- Keep SDK headers/source distinct from redistributable runtime DLLs. Copy runtime DLLs next to the executable only when configured and present.
- Add or validate a production/development Streamline runtime-mode switch when practical. Production is the shipping default: use production/non-watermarked runtimes, disable Streamline console/debug text, avoid development JSON/debug overlays, and log the selected mode. Development mode should point at development/non-production runtimes, enable SDK console or verbose logs when supported, leave Streamline debug text enabled, allow `sl.dlss_g.json` or equivalent debug visualization config when intentionally present, and log that development DLLs must not ship.
- Treat missing `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, and Vulkan-only dependencies such as `NvLowLatencyVk.dll` as clean fallback conditions.
- Query DLSS-G support and `DLSSGState` after the device/proxy path is valid. Log support/load status, state status, `numFramesToGenerateMax`, `numFramesActuallyPresented`, `minWidthOrHeight`, VSync support, Dynamic MFG support, and estimated VRAM only when intentionally queried.
- Add runtime options for Off, On, Auto only when supported/appropriate, Dynamic only when supported/appropriate, and generated-frame count or multiplier when exposed.
- Keep requested FG mode/count separate from effective configured mode/count and actual present-active state.
- Clamp requested generated-frame count to `DLSSGState::numFramesToGenerateMax`.
- Configure DLSS-G on swapchain/resource creation and settings changes, but do not report it active until a real frame with valid current-frame constants and required tags is presented.
- Avoid enabling FG for menus, loading, pause, cutscenes, resize, camera cuts, or other non-game frames that do not emit complete current-frame constants and tags. Prefer deferred-off behavior until tagged gameplay frames exist.
- Avoid redundant `slDLSSGSetOptions` calls. Apply options on settings/swapchain changes and real state transitions such as deferred-off to active tagged frame, or active to disabled.
- For every active frame, set common constants for that same app frame, tag depth and motion vectors for that same app frame, keep tagged resources valid until present, and ensure present consumes the same frame identity.
- Treat missing constants, missing/stale tags, mixed frame identity, support-query failure, feature-creation failure, missing runtime DLL, unsupported GPU/driver/HWS/backend, and present-hook failure as explicit fallback paths.
- Preserve Vulkan acquire/present semaphore correctness and image-index ownership. Preserve D3D12 backbuffer index correctness.
- Validate requested settings against effective runtime state. For VSync, frame limiters, fullscreen/windowed mode, FG mode/count, and present-active state, do not trust UI/config/command-line values alone; log or inspect the real present call, swapchain state, SDK state, and any UI/config path that can reapply saved settings after startup.
- Tag optional production-quality resources where available: HUDless color, UI alpha or UI color+alpha, backbuffer extent for subrects, and distortion field.
- Rebind or restore engine graphics state after Streamline work if the backend wrapper can disturb descriptors, pipelines, root signatures, viewports, scissors, barriers, or queues.
- Add targeted logs listed in [references/integration-checklist.md](references/integration-checklist.md), keeping per-frame logs behind a debug cvar or setting.

After implementation:

1. Read [references/validation-checklist.md](references/validation-checklist.md).
2. Build the game/application and fix compile, warning-as-error, link, shader/package, and runtime-load issues.
3. Run `git diff --check`.
4. Run with FG disabled and with FG requested/enabled.
5. Run a real-scene smoke test that exercises non-empty depth, non-empty motion vectors, camera motion, a moving object when available, the normal present path, and UI/HUD if present. Do not count an empty editor/menu viewport.
6. When a development runtime mode exists, run one development-mode scene with SDK debug visualization/logs enabled and one production-mode smoke confirming no development overlay or watermark is visible.
7. Capture screenshot/video or preserve logs and mark visual validation as blocked. Measure FPS/pacing where practical.
8. Perform a code-review pass over the integration. Call out unusual, fragile, incomplete, or counterintuitive parts before finalizing.

## Final Response

Report the following:

- Summary table: Build/static, runtime optional fallback, Streamline setup, DLSS-G functional activation, real-scene frame tagging, visual artifact validation, performance/pacing validation, and overall.
- Files changed.
- Integration quality level: MVP, production candidate, or reference quality.
- Runtime options/config/menu/console commands added.
- Build command and build result.
- Run command with FG disabled.
- Run command with FG requested/enabled.
- Real-scene validation command/map/script used.
- Runtime dependency notes.
- Production/development runtime mode, selected runtime path, and debug-visualization result when development mode exists.
- Requested mode/count vs effective mode/count vs actual present-active mode.
- `DLSSGState` summary.
- Project/application identifier result.
- Proxy/manual device setup result.
- Swapchain/present integration summary.
- Required and optional input tag tables.
- Frame-token/frame-identity evidence.
- Motion-vector producer-chain summary.
- Fallback behavior summary.
- Static validation result.
- Runtime smoke result.
- Visual capture result.
- Performance/pacing result if measured.
- Known limitations and follow-up PRs split into correctness, quality, validation, and optional reference-quality work.
