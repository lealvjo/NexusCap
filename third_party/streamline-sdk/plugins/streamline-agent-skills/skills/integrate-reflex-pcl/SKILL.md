---
name: integrate-reflex-pcl
description: Integrate and validate NVIDIA Streamline Reflex and PCL latency markers in a game engine using the Streamline SDK, Reflex/PCL headers, existing Streamline or NVAPI Reflex wrappers, render/input/simulation/present loop inspection, frame-token ownership, slReflexSleep placement, runtime fallback, and bundled integration/validation checklists. Use when an AI coding agent is asked to add, debug, verify, or code-review Reflex low-latency modes, Reflex sleep, PCL markers, latency marker ordering, frame tokens, frameLimitUs, Reflex UI availability, runtime toggles, or Reflex/PCL verification tooling in an engine.
---

# Reflex/PCL Integration

## Skill Source

Treat `skills/integrate-reflex-pcl/` as the shared skill source for Codex and Claude Code.

- Keep `SKILL.md` and `references/` portable Markdown with relative links.
- Keep frontmatter and links portable across Codex and Claude; do not add runtime-specific metadata, slash-command assumptions, or workstation-specific install paths.

## References

Read the bundled Reflex/PCL integration and validation checklists before editing:

- [references/integration-checklist.md](references/integration-checklist.md)
- [references/validation-checklist.md](references/validation-checklist.md)

## Final Response

Report the following:

- Summary: PASS / FAIL.
- Files changed.
- Build command and build result.
- Run commands for Off, On, and On + Boost.
- Runtime dependency notes and Streamline DLLs found/missing.
- Static validation result.
- Runtime validation result.
- Reflex/PCL verification tool result, if run.
- App Called Sleep result, if verified.
- Marker/frame-token evidence.
- Marker order evidence.
- ReflexState latency report result.
- Runtime toggle result.
- VSync/frame limiter/resize/fullscreen/threaded-renderer result.
- Unsupported GPU/runtime-missing result.
- Performance regression result, if measured.
- Remaining risks.
- Exact files/functions fixed.
- Exact files/functions still needing fixes.
