# AGENTS.md — Instructions for AI Coding Agents

Welcome! This repository (`Creo-Automation`) drives **PTC Creo Parametric** (`8.0` verified; `9–12` auto-adapted) on Windows x64 through a JSON-plan-driven C++17 **Pro/TOOLKIT** synchronous DLL (`src_c/`) and pure-stdlib Python automation (`setup.py`, `build_cpp.py`, `creobuild_env.py`).

If a user sent you this GitHub URL and asked you to **deploy, set up, or run Creo automated modeling**, follow this exact protocol:

---

## 1. Autonomous Onboarding & Deployment Protocol

1. **Check the user's Windows environment (machine-readable JSON)**:
   ```powershell
   python setup.py --check --json
   ```
2. **Interpret the diagnostic output**:
   - **`creo.creo_ok == false`**: Ask the user for their Creo Parametric install folder (e.g., `C:\Program Files\PTC\Creo 8.0.0.0`) and save it:
     ```powershell
     python setup.py --creo-root "<USER_CREO_PATH>" --save --check
     ```
   - **`creo.protoolkit_sdk_ok == false`**: Do **NOT** fabricate or download Pro/TOOLKIT headers/libs from external sources. Stop and instruct the user to run `<CREO_ROOT>\Parametric\bin\reconfigure.exe`, select **Creo TOOLKIT / API Toolkits**, apply the update, and tell you when done.
   - **`creo.license_valid == false`**: Ask the user for their license file path or FlexNet `port@host` server (e.g. `7788@localhost`) and save it:
     ```powershell
     python setup.py --license "<PORT@HOST_OR_DAT_PATH>" --save --check
     ```
   - **`msvc.ok == false`**: Install zero-UAC portable MSVC x64 + Windows SDK locally into `.tools/portable_msvc/` (ask or use `--mirror cn` in mainland China, `--mirror global` otherwise):
     ```powershell
     python scripts/install_portable_msvc.py --accept-license --mirror global
     ```
3. **Save detected config & run the 5-second smoke test**:
   ```powershell
   python setup.py --auto
   python build_cpp.py --plan plans/flanged_hub.json
   ```
   Verify the output prints `STATUS=OK`, `FAILED_COUNT=0`, `.inf SHA256 match`, and exits `0`.
4. **Optional showcase verification**:
   ```powershell
   python build_cpp.py --plan plans/turbofan_intake_fan.json
   ```
   (Use `--default-names` if the user wants localized Creo default feature names such as `Extrude 1` / `拉伸 1`.)

---

## 2. Where Modeling Knowledge Lives

- Read `.agents/skills/creo-parametric-automation/SKILL.md` for Pro/TOOLKIT constraints, 7-step workflow, and acceptance gates.
- Read `docs/PLAN_SCHEMA.md` before creating or editing any JSON plan under `plans/`.
- Read `docs/TROUBLESHOOTING.md` if Creo setup, unlocking, or regeneration fails.

---

## 3. Repository & Code Invariants (Do Not Break)

1. **Pure stdlib Python**: Keep `creobuild_env.py`, `setup.py`, `build_cpp.py`, and `scripts/install_portable_msvc.py` free of third-party `pip` dependencies.
2. **No hardcoded personal paths or secrets**: Never hardcode drive paths (`C:\...`, `D:\...`, `E:\...`) or license filenames in source files. All paths flow through `creobuild_env.py` and gitignored `creobuild.local.json`.
3. **Unicode / spaces path safety**: Never serialize file paths with `encoding="ascii"`; keep `compile_dll` invoking `cl.exe`/`link.exe` directly with captured `msvc_env`.
4. **Human-Like Model Tree Design (拟人化模型树)**:
   - Every generated model must remain clean and editable by CAD engineers in Creo UI: minimal datum skeletons, native single-feature evolution (`rotational_blend` / `helical_sweep` / `revolve` / `extrude` instead of dozens of discrete loft datum planes), natural order (base solid -> cuts -> `round`/`chamfer` -> `feature_group`/`axis_pattern` -> feature-scoped `set_appearance`), and no `Note_` clutter on assembly components (`rename_component_feature: false`).
5. **Pro/TOOLKIT Engine Rules (`src_c/`)**:
   - `creo_auto_modeler.cpp` & `creo_plan_core.hpp` must keep Sketch Intent Manager ordering (`ModeSet(TRUE)` -> entities/constraints/dims -> `ModeSet(FALSE)` -> `ProSectionAutodim` -> `ProSectionRegenerate`), `PRO_FEAT_PATTERN_HEAD` checks, `ProMdlVisibleSideTexturepropsSet` before `ProMdlVisibleSideTextureplacementpropsSet`, `ProSectionEpsilonSet` + `SketchEntityCache` for `rotational_blend`, and active-window guarantees for interactive/mechanism plans.
6. **Dual-phase verification**: Any modeling change must verify both `build` and isolated `verify` reopen (`PRO_REGEN_FORCE_REGEN`) with zero `!%CE`, `!%CW`, `失序`, or `out of sequence` lines in `trail.txt.*`.
7. **Clean unit tests**: Run `python -m unittest discover -s tests -v` after editing Python modules.
