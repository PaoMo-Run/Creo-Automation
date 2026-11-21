# Troubleshooting Guide

## 1. Environment & SDK Setup Issues

### `Missing Pro/TOOLKIT headers` or `Missing Pro/TOOLKIT x64 libraries`
- **Cause**: When installing Creo Parametric, the optional **Creo TOOLKIT** (or **API Toolkits**) component is unchecked by default.
- **Fix**:
  1. Close Creo completely.
  2. Run `<CREO_ROOT>\Parametric\bin\reconfigure.exe` (or open PTC Installation Assistant from Windows Start Menu).
  3. On the **Customize** / feature selection screen, expand **Creo Parametric -> API Toolkits** and check **PTC Creo TOOLKIT** (`Common Files\protoolkit` + `protk_unlock.exe`).
  4. Apply changes, then re-run `python setup.py --check`.

### `protk_unlock.exe failed`
- **Cause**: Either `PTC_D_LICENSE_FILE` is not pointing to a valid license file / FlexNet `port@host` server, or your current Creo license does not include the `PRO_TOOLKIT` / unlock entitlement.
- **Fix**:
  - Check what `python setup.py --check` printed for `PTC License`.
  - Test manually with `python setup.py --license "7788@your-server" --check` or `--license "C:\path\to\license.dat" --save`.
  - Verify your license contains Pro/TOOLKIT API support (`CREOPMA_TOOLKIT` / `PRO_TOOLKIT`).

### `No MSVC x64 C++ compiler / Windows SDK found`
- **Cause**: Visual Studio C++ Build Tools are not installed or not discovered automatically.
- **Fix**:
  - **Zero-UAC Portable Option**: Run:
    ```powershell
    python scripts/install_portable_msvc.py --accept-license
    # Or in mainland China:
    python scripts/install_portable_msvc.py --accept-license --mirror cn
    ```
    This installs MSVC + Windows SDK locally under `.tools/portable_msvc/` without requiring Administrator UAC prompts or modifying `C:\`.
  - **Standard Visual Studio Option**: Install **Visual Studio 2022 Build Tools** (or Community) with the **Desktop development with C++** workload, or pass `--msvc-setup "C:\path\to\vcvars64.bat"` to `setup.py`.

### `Could not auto-detect any Creo Parametric installation`
- **Fix**: Pass your installation folder explicitly once and save it to `creobuild.local.json`:
  ```powershell
  python setup.py --creo-root "C:\Program Files\PTC\Creo 8.0.0.0" --save --check
  ```

---

## 2. Creo Runtime & Modeling Failures

### Creo blocks unsigned DLL with a security dialog
- **Cause**: Creo 8.0+ blocks unsigned Pro/TOOLKIT DLLs unless `open_protk_unsigned_apps always` is present in `config.pro`.
- **Fix**: `build_cpp.py` automatically writes `open_protk_unsigned_apps always` into the isolated sandbox `config.pro`. If you run the DLL manually outside `build_cpp.py`, make sure that option is set in your working `config.pro`.

### `trail.txt.*` reports `失序` or `out of sequence`
- **Cause**: A dashboard macro (`solid_blend`, `rotational_blend`, `helical_sweep`) fired without an active template window or hit an unexpected UI state.
- **Fix**:
  - Ensure no leftover `xtop.exe` or `nmsd.exe` processes were holding locks before starting.
  - Confirm the JSON plan uses a valid `template` present in `<CREO_ROOT>\Common Files\templates`.
  - If using Creo 9/10/11/12, check the latest `trail.txt.*` inside `build_cpp/stage/` or `build_cpp/verify/` to see which UI command differed from Creo 8.0.

### `SEC_FAIL` or `PRO_TK_BAD_CONTEXT (-8)` during sketch regeneration
- **Cause**: Calling `ProSectionAutodim` / `ProSectionRegenerate` while `ProSectionIntentManagerModeSet(sec, PRO_B_TRUE)` is still active, or creating degenerate tiny arcs under a coarse `epsilon` on far-from-origin sketches.
- **Fix**: Keep Intent Manager open only while adding entities/constraints/dimensions, close it before `sec_autodim_and_regen`, and set `"epsilon": 0.01` on rotational/offset sketches far from the origin.

### `ERROR_CUT_NO_MATERIAL`
- **Cause**: A `"mode": "cut"` operation did not intersect existing solid material (volume reduction `<= 1e-6 mm³`), or `side` extruded away from the solid.
- **Fix**: Switch `"side"` between `"side_two"` and `"side_one"`, verify `attach_body: true` on the base solid, and verify sketch coordinates in 3D/RZ match the solid body.

### `xtop.exe - 系统错误`: missing `true_solid_wrappersmt.dll`, `cxx14mt.dll`, `gen_cloudmt.dll`, or `xalancmt.dll` (`0xC0000135`)
- **Cause**:
  - A stale `PRO_DIRECTORY`, `PRO_MACHINE_TYPE`, or `CREO_COMMON_FILES` environment variable is set in the shell (for example `PRO_DIRECTORY` pointing to `<CREO_ROOT>` instead of `<CREO_ROOT>\Common Files`), causing `parametric.psf` to look in the wrong directory;
  - `xtop.exe` was invoked directly instead of `parametric.exe parametric.psf`, or `<CREO_ROOT>\Common Files\x86e_win64\lib` was missing from `PATH`;
  - An AI coding agent ran `build_cpp.py` inside a restricted sandbox (such as Codex `workspace-write`) and rewrote the launcher scripts after Creo failed inside the sandbox.
- **Fix**:
  - Use the unmodified `build_cpp.py` (`git checkout -- build_cpp.py setup.py src_c` if an agent modified them). `build_cpp.py` clears stale `PRO_DIRECTORY` / `PRO_MACHINE_TYPE` / `CREO_COMMON_FILES`, prepends Creo's `Common Files\x86e_win64\{lib,deflib,obj}`, `Common Files\libs\dfor\lib`, and `Parametric\bin` directories to `PATH`, and launches `parametric.exe` with `parametric.psf`.
  - If using Codex CLI/IDE, grant Full Access / Windows host execution when running Creo, or run `.\setup.ps1` and `python build_cpp.py --plan plans/flanged_hub.json` in a normal Windows PowerShell terminal.
  - Note on visible GUI window: interactive commands executed under the normal Windows desktop session launch Creo on the desktop (rather than an isolated agent virtual desktop), which is normal when `parametric.exe` builds interactive features or assemblies.

### `python` is not recognized in PowerShell (Python installed only in `%LocalAppData%\Programs\Python`)
- **Cause**: Windows Store / user-local Python installations are sometimes omitted from the system `PATH` and have no `py.exe` launcher.
- **Fix**: Run `.\setup.ps1` (which probes `%LocalAppData%\Programs\Python\Python3*\python.exe` and `%ProgramFiles%\Python*\python.exe` automatically), or call your Python executable by full path, e.g. `& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" build_cpp.py --plan plans/flanged_hub.json`.
