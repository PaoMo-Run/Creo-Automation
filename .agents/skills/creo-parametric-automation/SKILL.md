---
name: creo-parametric-automation
description: >-
  Creo Parametric (8.0 verified; 9-12 auto-adapted) C/C++17 Pro/TOOLKIT JSON-plan-driven
  parametric automated modeling, human-like native feature trees, constrained 2D/3D sketching,
  complex solid & surface features (extrude, revolve, helical_sweep, rotational_blend,
  solid_blend, sweep, loft, mirror, pattern), mechanism joints (pin/slider/cylindrical),
  servo motors (PRO_FEAT_PM_MOTOR) & mechanism analysis, B-rep coloring & embedded cylindrical
  textures, cross-environment setup, and isolated reopen verification workflow.
  Use when creating or modifying Creo 3D parts/assemblies automatically, debugging ProSection /
  ElementTree / Asmcomp / Mechanism errors, or setting up Pro/TOOLKIT + MSVC toolchains.
---

# Creo Parametric C++17 Pro/TOOLKIT Automated Modeling Guide

## 1. Environment Discovery & First-Run Deployment

Before generating or compiling any CAD model on a new Windows machine, verify the environment:

```powershell
python setup.py --check --json
```

1. **Creo Installation & Pro/TOOLKIT SDK**:
   - `creobuild_env.py` auto-discovers Creo (`8.0–12.x`) from `--creo-root`, `CREOBUILD_CREO_ROOT`, `creobuild.local.json`, Windows Registry, `PATH`, and common PTC installation folders.
   - Required SDK files under `<CREO_ROOT>\Common Files\protoolkit`: `includes\ProToolkit.h`, `x86e_win64\obj\{protk_dll_NU.lib, ucore.lib, udata.lib}`, and `Common Files\x86e_win64\obj\protk_unlock.exe`.
   - **If `protoolkit_sdk_ok` is `false`**: stop and instruct the user to run `<CREO_ROOT>\Parametric\bin\reconfigure.exe` and enable **Creo TOOLKIT / API Toolkits**.
2. **License Discovery**:
   - Automatically checks `--license` -> `PTC_D_LICENSE_FILE` -> `LM_LICENSE_FILE` -> `parametric.psf` (`ENV=PTC_D_LICENSE_FILE...` / `ENV=LM_LICENSE_FILE...`), supporting both `.dat` files and `port@host` license servers.
3. **MSVC x64 C++17 Toolchain**:
   - Automatically checks `--msvc-setup` -> `creobuild.local.json` -> `.tools\portable_msvc\setup_x64.bat` -> active developer `PATH` -> `vswhere.exe` (VS 2019/2022) -> common portable locations.
   - **If `msvc.ok` is `false`**: run the zero-UAC installer:
     ```powershell
     python scripts/install_portable_msvc.py --accept-license --mirror global
     # Or in mainland China:
     python scripts/install_portable_msvc.py --accept-license --mirror cn
     ```
4. **Smoke Test**:
   ```powershell
   python build_cpp.py --plan plans/flanged_hub.json
   ```

---

## 2. Human-Like Model Tree Principles (拟人化特征树准则)

Every generated `.prt` / `.asm` must look as if authored by an experienced mechanical engineer inside Creo UI, not generated as an uneditable mesh or exploded plane list:

1. **Minimal Datum Skeleton**: Use `RIGHT`, `TOP`, `FRONT` and only the minimum necessary offset/angled datums and axes. Never create dozens of auxiliary datum planes + discrete loft slices for swept or twisted geometry.
2. **Single Native Parametric Features & Batch Feature Mirroring**: For twisted blades, spiral grooves, or revolved bodies, always prefer native `rotational_blend` (internal multi-angle sections), `helical_sweep` (internal profile + pitch + section), `revolve`, `extrude`, and `sweep` so an engineer can right-click *Edit Definition* directly in Creo to adjust pitch, angle, or sketch dimensions. For symmetric left/right/front/rear solid or cut features, model one side cleanly and batch-mirror by finish color via `op: "mirror"` (`ProCmdFtMirror` with `features`/`expand_groups`) rather than duplicating feature trees.
3. **Natural Engineering Feature Order**:
   - Build primary solid bodies attached to `DefaultBody` first -> functional cutouts/grooves -> `round` / `chamfer` -> `feature_group` and `axis_pattern`.
   - Apply local functional finishes via `set_appearance` with `scope: "features"` or spatial `surfaces` filters (never raw face IDs) so colors track feature regeneration cleanly.
4. **Clean Tree Naming & Zero `Note_` Clutter**: Give part features clear descriptive names (`name_zh` / `name`, or omit via `--default-names`), and never call `ProModelitemNameSet` on `PRO_FEAT_COMPONENT` (`rename_component_feature: false` by default) so assembly trees stay clean without `Note_` annotations.

---

## 3. Seven-Step Modeling & Verification Workflow

1. **Feature Tree & Geometry Abstraction**:
   - Design the minimal datum skeleton and native feature sequence following Section 2.
   - For closed multi-arc profiles (e.g. twisted fan blades), solve `G1` tangent arcs analytically (tangent gap `< 1e-4°`, closure gap `< 1e-5 mm`) to avoid over-dimension/pseudo-tangent warnings.
2. **Author JSON Modeling Plan (`plans/*.json`)**:
   - Declare part operations sequentially (`datum_plane`, `datum_axis`, `extrude`, `revolve`, `round`, `chamfer`, `feature_group`, `axis_pattern`, `sketched_curve`, `sweep`, `solid_blend`, `rotational_blend`, `helical_sweep`, `surface_loft`, `flat_fill`, `quilt_merge`, `solidify`, `mirror`, `set_appearance`, `set_texture`).
   - For assemblies (`doc_type: "assembly"`), build child part plans via `components_plans`, assemble with `assemble_component` (including closed-loop `additional_sets`), and optionally add top-level `motors` + `mechanism_analysis`.
   - See `docs/PLAN_SCHEMA.md` for full JSON syntax.
3. **Compile, Unlock & Sandbox Initialization**:
   - `build_cpp.py` terminates stale `xtop.exe`/`nmsd.exe`, compiles `src_c/creo_auto_modeler.cpp` with `/std:c++17`, unlocks `creobuild.dll` via `protk_unlock.exe -c`, and stages isolated `creotk.dat` + `config.pro` (`open_protk_unsigned_apps always`, `save_texture_with_model yes`, `search_path .`).
   - Plans containing `solid_blend`, `rotational_blend`, `helical_sweep`, `motors`, or `mechanism_analysis` launch Creo with an active CLI template window (`needs_active_ui`).
4. **In-Session Build Execution**:
   - `user_initialize()` registers UI commands and defers execution via `ProMacroLoad("~ Command `CreoBuildExec` ;")` until Creo's event loop is live.
   - Interactive features (`ProCmdFtPBlend`, `ProCmdFtRBlend`, `ProCmdFtHelicalSweep`) pause step execution, populate references/sketches via `CreoMacroPickRef` / `CreoMacroFillSketch` + `SketchEntityCache`, and resume via `CreoBuildResume`.
5. **Assembly, Mechanism Joints & Servo Motors**:
   - `pin` / `slider` / `cylindrical` / `bearing` / `ball` / `planar` components build pure `ProElementAlloc` trees (no `ProFeatureElemtreeExtract` mixing) and never call `ProModelitemNameSet` on `PRO_FEAT_COMPONENT` by default (preventing `Note_` annotation clutter).
   - Mechanism setups run `PRO_REGEN_FORCE_REGEN`, enter Mechanism mode (`page_Applications_control_btn` + `ProCmdUtilAppMech 1`), create `PRO_FEAT_PM_MOTOR` features, run `PMechCmdAnalyses -> runBtn -> OKButton` to persist `AnalysisDefinition1`, and log post-analysis poses before restoring and saving.
6. **Isolated Reopen & Force-Regeneration Verification**:
   - `build_cpp.py` copies `.prt.1` / `.asm.1` into a clean `build_cpp/verify/` directory **without external texture `.jpg` files**, reopens the model, executes `ProSolidRegenerate(..., PRO_REGEN_FORCE_REGEN)`, verifies `failed=0`, checks `ProTexturePathGet(..., &was_embedded, ...)` when textures are used, and compares `.inf` SHA256 against the build phase.
7. **Trail Hygiene & Visual Inspection**:
   - Fails automatically if `trail.txt.*` contains `!%CE`, `!%CW`, `失序`, or `out of sequence`.
   - Inspect generated `*_default.jpg`, `*_front.jpg`, `*_top.jpg` (and `*_right.jpg`) screenshots before delivering.

---

## 4. Critical Pro/TOOLKIT Implementation Invariants

- **Sketch Intent Manager Ordering**:
  - Never call `ProSectionAutodim`, `ProSectionRegenerate`, or `ProSectionSolve` while `ProSectionIntentManagerModeSet(sec, PRO_B_TRUE)` is active (`PRO_TK_BAD_CONTEXT = -8`).
  - Always follow: `ModeSet(TRUE)` -> add entities/constraints/dimensions -> `ModeSet(FALSE)` -> `ProSectionAutodim` -> `ProSectionRegenerate`.
- **`Pro2dArcdef` CCW Sweep Consistency**:
  - Creo 2D arcs always sweep counter-clockwise from `start_angle` to `end_angle`. If projecting 3D points swaps angles, track the swap and remap `PRO_ENT_START` / `PRO_ENT_END` before `PRO_CONSTRAINT_SAME_POINT`.
- **Body Attachment & Closed Cuts**:
  - Attach initial solid features to `DefaultBody` (`attach_body: true` via `ProSolidDefaultBodyGet`).
  - Do not force-overwrite `PRO_E_STD_MATRLSIDE` on closed-loop cuts (`extrude`, `revolve`, `sweep`).
- **Pattern Member Expansion**:
  - Only expand `ProPatternMembersGet` when `ftype == PRO_FEAT_PATTERN_HEAD` (never compare `ftype == PRO_FEAT_PATTERN`, which shares enum value `0` with `PRO_FEAT_FIRST_FEAT`).
  - Filter feature edges/surfaces against live solid body B-rep items (`ProSolidBodySurfaceVisit`).
- **Cylindrical Texture Persistence**:
  - Never rely solely on `ProSurfaceSideTextureplacementpropsSet` for cylindrical placement. Construct a root `ProAsmitem` and call `ProMdlVisibleSideTexturepropsSet` followed by `ProMdlVisibleSideTextureplacementpropsSet`.
- **Rotational Blend Sketch Isolation**:
  - Call `ProSectionEpsilonSet(sec, 0.01)` before adding entities near far rotation axes.
  - Never call `ProSectionEntityGet` on deflected sections (`Sec k`, `k >= 1`) before `ProSectionActiveSet`; use cached `SketchEntityCache` 2D points and analytical 3D section origin shifts (`compute_rotblend_section_shift`).
- **Mechanism Active-Window Guard**:
  - Assemblies containing `motors` or `mechanism_analysis` require an active visible model window during both build and `verify` (`ProWindowCurrentSet` + `ProWindowActivate` + `ProWindowRepaint` before `PRO_MODEL_INFO` export).
- **Simulated Manual Feature Mirroring (`ProCmdFtMirror`) & Sketch Limits** (see [feature_mirror_workflow.md](./references/feature_mirror_workflow.md)):
  - `op: "mirror"` routes `"features"` / `"feature"` operations to `schedule_feature_mirror`, which expands `ctx.group_member_ids` (when `expand_groups: true`), pre-selects features into `ProSelbuffer` *before* launching `ProCmdFtMirror`, records created `PRO_FEAT_GROUP_HEAD` + member IDs into `ctx.group_member_ids` on `CreoBuildResume` (settling via a 120ms UI timer), and triggers `PRO_REGEN_FORCE_REGEN` before save.
  - Keep collinear narrow rectangles per `IntentManager` sketch `<= 3` (split 4–5 into `2+2` or `3+2` to avoid `ProSectionAutodim st=-1 n_err=0`), keep sloped blend end-caps overlapping walls by `>= 0.5–2 mm`, and model directional text/numbers independently on each side rather than mirroring.

---

## 5. Mandatory Acceptance Gates

1. `failed=0` on all features (`ProSolidFailedfeaturesList`) and expected volume/area progression.
2. Zero `!%CE`, `!%CW`, `失序`, or `out of sequence` in both build and reopen `trail.txt.*`.
3. Reopen `PRO_REGEN_FORCE_REGEN` `.inf` SHA256 matches build `.inf` SHA256 (for `ProCmdFtMirror` models, `build_cpp.py` applies `normalize_mirror_inf` to align Creo 8.0's reopen row numbering).
4. Multi-view `.jpg` renders visually inspected for contour, holes, orientation, and finish.
5. For textured models: `TEX_EMBED ... embedded=1` verified in an isolated folder without external `.jpg` files.
6. Human-like, engineer-editable model tree: clean datum skeleton, native `rotational_blend`/`helical_sweep`/`axis_pattern`/`ProCmdFtMirror` hierarchy, feature-scoped coloring, and `0` `Note_` items in assemblies.
7. No leftover `parametric.exe`, `xtop.exe`, or `nmsd.exe` processes after completion.
