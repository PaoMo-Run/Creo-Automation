# AGENTS.md — Instructions for AI Coding Agents

Welcome! This repository (`Creo-Automation`) drives **PTC Creo Parametric** (`8.0` verified; `9–12` auto-adapted) on Windows x64 through a JSON-plan-driven C++17 **Pro/TOOLKIT** synchronous DLL (`src_c/`) and pure-stdlib Python automation (`setup.py`, `build_cpp.py`, `creobuild_env.py`).

If a user sent you this GitHub URL and asked you to **deploy, set up, or run Creo automated modeling**, follow this exact protocol:

---

## 1. Autonomous Onboarding & Deployment Protocol

1. **Check the user's Windows environment (machine-readable JSON)**:
   If `python` is not on `PATH`, probe `%LocalAppData%\Programs\Python\Python3*\python.exe` (or `py -3`) automatically.
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
   > **Codex / Restricted Sandbox Note**: Creo Parametric is a multi-process native Windows CAD application (`parametric.exe` -> `nmsd.exe` -> `xtop.exe`) requiring cross-directory DLL/license access and GUI event loops. **Never run `build_cpp.py` inside Codex's restricted `workspace-write` sandbox** (`filesystem=restricted`), and **never modify `build_cpp.py` to launch `xtop.exe` directly or pass `-g:no_graphics`** (which triggers missing-DLL `0xC0000135` crashes and breaks Pro/TOOLKIT graphics/macros). Request full host execution or instruct the user to run `build_cpp.py` in a standard Windows PowerShell. Note that on a standard desktop session (unlike isolated virtual desktops), Creo's window briefly appears for ~10–30s and closes automatically.
4. **Optional showcase verification**:
   ```powershell
   python build_cpp.py --plan plans/turbofan_intake_fan.json
   ```
   (Use `--default-names` if the user wants localized Creo default feature names such as `Extrude 1` / `拉伸 1`.)

---

## 2. Where Modeling Knowledge Lives

- Read `.agents/skills/creo-parametric-automation/SKILL.md` (and `references/feature_mirror_workflow.md`) for 3D Pro/TOOLKIT constraints, 7-step workflow, simulated manual mirroring, `default_view_camera`, and `hide_auxiliary_layers`.
- Read `.agents/skills/creo-engineering-drawing/SKILL.md` for the GB/T A3 2D drawing engine (`creo_drawing_engine.hpp`, four-pass views, `chfntf`, PDF/DWG exports).
- Read `.agents/skills/creo-ui-demo-recording/SKILL.md` for desktop UI demo automation (`scripts/human_ui.py`, `scripts/probe_creo_ui.py`, Bezier/overshoot cursor, `Ctrl+Middle` zoom, Win11 MS Pinyin, `F2` pause & FFmpeg concat).
- Read `docs/PLAN_SCHEMA.md` before creating or editing any JSON plan under `plans/`.
- Read `docs/TROUBLESHOOTING.md` if Creo setup, unlocking, or regeneration fails.

---

## 3. Repository & Code Invariants (Do Not Break)

1. **Pure stdlib Python**: Keep `creobuild_env.py`, `setup.py`, `build_cpp.py`, `scripts/install_portable_msvc.py`, `scripts/probe_creo_ui.py` (without `--dump-uia`), and core `scripts/human_ui.py` (`HumanDriver`/`PauseController`/`FFmpegRecorder`) free of required third-party `pip` dependencies (`UIADriver` lazy-imports optional `comtypes` only when invoked).
2. **No hardcoded personal paths or secrets**: Never hardcode drive paths (`C:\...`, `D:\...`, `E:\...`) or license filenames in source files. All paths flow through `creobuild_env.py` and gitignored `creobuild.local.json`.
3. **Unicode / spaces path safety**: Never serialize file paths with `encoding="ascii"`; keep `compile_dll` invoking `cl.exe`/`link.exe` directly with captured `msvc_env`.
4. **Human-Like Model Tree Design (拟人化模型树)**:
   - Every generated model must remain clean and editable by CAD engineers in Creo UI: minimal datum skeletons, native single-feature evolution (`rotational_blend` / `helical_sweep` / `revolve` / `extrude` instead of dozens of discrete loft datum planes), natural order (base solid -> cuts -> `round`/`chamfer` -> `feature_group`/`axis_pattern` -> feature-scoped `set_appearance`), `hide_auxiliary_layers` persistent hiding for clean deliverable trees, and no `Note_` clutter on assembly components (`rename_component_feature: false`).
5. **Pro/TOOLKIT Engine Rules (`src_c/`)**:
   - `creo_auto_modeler.cpp` & `creo_plan_core.hpp` & `creo_drawing_engine.hpp` must keep Sketch Intent Manager ordering (`ModeSet(TRUE)` -> entities/constraints/dims -> `ModeSet(FALSE)` -> `ProSectionAutodim` -> `ProSectionRegenerate`), `PRO_FEAT_PATTERN_HEAD` checks, `ProMdlVisibleSideTexturepropsSet` before `ProMdlVisibleSideTextureplacementpropsSet`, `ProSectionEpsilonSet` + `SketchEntityCache` for `rotational_blend`, `hide_auxiliary_ref_features` (`ProModelitemHide` + `ProLayerDisplaystatusSave`), `apply_plan_default_view`, four-pass 2D drawing view ordering with CLI `.drw.1` context, and active-window guarantees for interactive/mechanism plans.
6. **Dual-phase verification**: Any modeling change must verify both `build` and isolated `verify` reopen (`PRO_REGEN_FORCE_REGEN`) with zero `!%CE`, `!%CW`, `失序`, or `out of sequence` lines in `trail.txt.*`.
7. **Clean unit tests**: Run `python -m unittest discover -s tests -v` after editing Python modules.
