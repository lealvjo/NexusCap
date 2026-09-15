---
name: authoring-dlss-agent-skills
description: Maintain and validate the Streamline DLSS Agent Skills package itself, including shared skill sources, route-specific DLSS/Reflex checklist edits, plugin manifest prompts, shared workflow text, package.bat copy rules, BOM entries, and release packaging. Use when an AI coding agent is asked to add, review, or release changes to plugins/streamline-agent-skills rather than integrate DLSS into a game.
---

# Authoring DLSS Agent Skills

## Skill Source

Treat `skills/authoring-dlss-agent-skills/` as the shared skill source for Codex and Claude Code.

- Keep `SKILL.md` portable Markdown with relative links.
- Keep frontmatter and links portable across Codex and Claude; do not add runtime-specific metadata, slash-command assumptions, or workstation-specific install paths.

## Core Workflow

Use this skill for changes to the skill package, not for integrating DLSS into a game. For target-engine integration work, route to `integrate-dlss-sr`, `integrate-dlss-rr`, `integrate-dlss-fg`, or `integrate-reflex-pcl`.

Before editing:

1. Read `AGENTS.md` for package orientation, SDK routes, shared-source rules, and packaging expectations.
2. Identify whether the change touches skill workflow text, route-specific references, shared sources, scripts, manifest source/overlays, package copy rules, or BOMs.
3. Inspect shared `skills/`, package manifests, and the validator before deciding whether a change is skill-level or package-level.

During implementation:

- Edit `skills/<skill>/SKILL.md` directly for skill workflow text.
- Keep `skills/<skill>/SKILL.md` portable across Codex and Claude Code; do not create runtime-specific copies.
- Edit `plugin-manifest-source.json` for shared plugin metadata and Codex/Claude overlay changes.
- When changing shared Reflex/PCL workflow text, update the shared source instead of duplicating it into multiple `SKILL.md` files.
- Do not add marker-only strings, comments, or fixture tokens as evidence. Skill validation must reward real integration behavior and real maintenance invariants.

Validation environment:

- Package validation needs PowerShell plus this repository checkout.
- Validate package contents when changing packaging metadata.
- Windows headed jobs are still required for full real Streamline sample build/run/dump validation.

## Final Response

Report the following:

- Skill trees, references, manifests, scripts, or CI files changed.
- Shared-source decisions, including any intentional loader-specific manifest differences.
- Package/BOM changes, if any.
- Remaining validation gaps or risks.
