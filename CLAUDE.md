# CLAUDE.md

This project (`Creo-Automation`) provides a C++17 Pro/TOOLKIT JSON-plan-driven automation engine for PTC Creo Parametric on Windows x64, paired with pure-stdlib Python auto-discovery and setup scripts.

- Read [AGENTS.md](AGENTS.md) for the autonomous environment check, portable MSVC installation, smoke-test protocol, sandbox warning, and engineering invariants.
- Read [.agents/skills/creo-parametric-automation/SKILL.md](.agents/skills/creo-parametric-automation/SKILL.md), [.agents/skills/creo-parametric-automation/references/feature_mirror_workflow.md](.agents/skills/creo-parametric-automation/references/feature_mirror_workflow.md), and [.agents/skills/creo-parametric-automation/references/mechanism_ui_video_recording.md](.agents/skills/creo-parametric-automation/references/mechanism_ui_video_recording.md) for the full Creo modeling, Pro/TOOLKIT, and desktop UI recording workflows.
- Read [docs/PLAN_SCHEMA.md](docs/PLAN_SCHEMA.md) when authoring new `plans/*.json` models.

Quick start for agents:
1. Run `python setup.py --check --json`
2. Resolve any missing `creo`, `protoolkit_sdk`, `license`, or `msvc` (`python scripts/install_portable_msvc.py --accept-license`)
3. Run `python setup.py --auto`
4. Verify with `python build_cpp.py --plan plans/flanged_hub.json`
5. Run Python unit tests with `python -m unittest discover -s tests -v`
