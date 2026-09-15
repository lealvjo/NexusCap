# Streamline Agent Skills

This package contains NVIDIA Streamline DLSS and Reflex skills for coding agents. Use it when the work is about integrating DLSS/Reflex into a target renderer, or maintaining the skills package itself.

## Skills

- `integrate-dlss-sr`: DLSS Super Resolution, upscaling, render/output dimensions, jitter, motion vectors, exposure, and UI composition order.
- `integrate-dlss-rr`: DLSS Ray Reconstruction, ray/path-traced denoising, guide buffers, hit distance, denoiser ordering, and RR runtime toggles.
- `integrate-dlss-fg`: DLSS Frame Generation, present/swapchain integration, UI alpha, pacing, fences, and frame-generation toggles.
- `integrate-reflex-pcl`: Reflex and PCL latency marker ordering, low-latency mode, simulation/render/present markers, and latency validation.
- `authoring-dlss-agent-skills`: maintenance of this plugin, including shared skill sources, manifests, references, packaging, and release files.

## SDK Routes

Each integration skill must use NVIDIA Streamline directly or through the target engine's existing Streamline wrapper or platform abstraction.

Prefer the Streamline route already present in the target engine. Do not replace an engine-owned abstraction with a separate Streamline path unless the task explicitly requires that migration.

## Maintenance Rules

- Treat `skills/` as the shared `SKILL.md` source tree for Codex and Claude Code.
- Keep `SKILL.md` frontmatter, links, and references portable across both loaders; do not add runtime-specific metadata or workstation-specific install paths.
- Use route-specific checklists for concrete integration steps. Keep `SKILL.md` focused on workflow, evidence requirements, and references.

## Authoring Environment

- Runtime-source and package-metadata validation needs PowerShell plus this repository checkout.
- New or substantially revised skills should follow the Codex `skill-creator` guidance. Plugin scaffold or manifest changes should follow the Codex `plugin-creator` guidance.
- Full real-sample validation is Windows-only: it needs the headed Windows runner, Visual Studio/CMake toolchain, D3D12 execution, the packaged Streamline SDK, and CI-provided credentials.
