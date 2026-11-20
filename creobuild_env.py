#!/usr/bin/env python3
"""Environment auto-discovery and configuration helpers for Creo-Automation.

Pure Python standard library module (zero third-party dependencies) that locates:
  1. PTC Creo Parametric (8.0 - 12.x) installation & Pro/TOOLKIT SDK.
  2. PTC license configuration (CLI/env/config -> parametric.psf parsing).
  3. MSVC x64 C++17 toolchain (portable setup_x64.bat, active PATH, or vswhere).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import string
import subprocess
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

try:
    import winreg  # type: ignore[import-not-found]
except ImportError:
    winreg = None  # Non-Windows fallback for unit tests

ROOT = Path(__file__).resolve().parent
LOCAL_CONFIG_NAME = "creobuild.local.json"


@dataclass
class CreoInstallInfo:
    """Resolved Creo Parametric installation paths and health checks."""

    creo_root: str = ""
    version: str = ""
    major_version: int | None = None
    creo_exe: str = ""
    creo_psf: str = ""
    ptk_inc: str = ""
    ptk_obj: str = ""
    protk_unlock: str = ""
    templates_dir: str = ""
    license_value: str = ""
    license_source: str = ""
    license_valid: bool = False
    protoolkit_sdk_ok: bool = False
    creo_ok: bool = False
    source: str = ""
    issues: list[str] = field(default_factory=list)


@dataclass
class MsvcToolchainInfo:
    """Resolved MSVC x64 build environment."""

    ok: bool = False
    kind: str = ""  # "portable", "vswhere", "env", "explicit"
    setup_bat: str = ""
    setup_args: list[str] = field(default_factory=list)
    cl_exe: str = ""
    vs_install_path: str = ""
    source: str = ""
    issues: list[str] = field(default_factory=list)


def load_local_config(config_path: Path | None = None) -> dict[str, Any]:
    """Load saved local environment overrides if present."""
    path = config_path or (ROOT / LOCAL_CONFIG_NAME)
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_local_config(data: dict[str, Any], config_path: Path | None = None) -> Path:
    """Persist resolved paths into creobuild.local.json (gitignored)."""
    path = config_path or (ROOT / LOCAL_CONFIG_NAME)
    clean = {k: v for k, v in data.items() if v is not None and v != ""}
    path.write_text(json.dumps(clean, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


def parse_psf_license(psf_path: Path) -> str | None:
    """Extract PTC_D_LICENSE_FILE or LM_LICENSE_FILE entries from a Creo .psf file."""
    if not psf_path.is_file():
        return None
    try:
        lines = psf_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None

    ptc_entries: list[str] = []
    lm_entries: list[str] = []
    pattern = re.compile(r"^\s*ENV\s*=\s*(PTC_D_LICENSE_FILE|LM_LICENSE_FILE)\s*(?:[+-]?=)\s*(.*?)\s*$", re.IGNORECASE)

    for raw in lines:
        stripped = raw.strip()
        if not stripped or stripped.startswith("//") or stripped.startswith("#"):
            continue
        m = pattern.match(raw)
        if not m:
            continue
        var_name = m.group(1).upper()
        val = m.group(2).strip().strip('"')
        if not val:
            continue
        target = ptc_entries if var_name == "PTC_D_LICENSE_FILE" else lm_entries
        for item in val.split(";"):
            entry = item.strip()
            if entry and entry not in target:
                target.append(entry)

    chosen = ptc_entries or lm_entries
    return ";".join(chosen) if chosen else None


def is_valid_license_spec(spec: str) -> bool:
    """Return True if at least one license entry is a port@host server or existing file."""
    if not spec or not spec.strip():
        return False
    for raw_entry in spec.split(";"):
        entry = raw_entry.strip().strip('"')
        if not entry:
            continue
        if "@" in entry:
            host_part = entry.split("@", 1)[1].strip()
            if host_part:
                return True
        elif Path(entry).is_file():
            return True
    return False


def detect_creo_version(creo_root: Path) -> tuple[str, int | None]:
    """Infer Creo version string and major version integer from install folder or files."""
    m = re.search(r"Creo\s*(\d+(?:\.\d+)*)", creo_root.name, re.IGNORECASE)
    if not m:
        m = re.search(r"Creo\s*(\d+(?:\.\d+)*)", str(creo_root), re.IGNORECASE)
    if m:
        ver = m.group(1)
        major = int(ver.split(".", 1)[0])
        return ver, major

    psf = creo_root / "Parametric" / "bin" / "parametric.psf"
    if psf.is_file():
        try:
            txt = psf.read_text(encoding="utf-8", errors="replace")
            pm = re.search(r"Creo\s+(\d+(?:\.\d+)*)", txt, re.IGNORECASE)
            if pm:
                ver = pm.group(1)
                return ver, int(ver.split(".", 1)[0])
        except OSError:
            pass
    return "", None


def _select_psf_file(bin_dir: Path, psf_name: str | None = None) -> Path | None:
    if psf_name:
        cand = Path(psf_name)
        if not cand.is_absolute():
            cand = bin_dir / psf_name
        if cand.is_file():
            return cand
    default_psf = bin_dir / "parametric.psf"
    if default_psf.is_file():
        return default_psf
    if bin_dir.is_dir():
        psfs = sorted(bin_dir.glob("*.psf"))
        if psfs:
            return psfs[0]
    return None


def inspect_creo_installation(
    creo_root: Path,
    *,
    psf_name: str | None = None,
    explicit_license: str | None = None,
    source: str = "detected",
) -> CreoInstallInfo:
    """Validate Creo installation, Pro/TOOLKIT SDK files, templates, and license."""
    info = CreoInstallInfo(creo_root=str(creo_root), source=source)
    ver, major = detect_creo_version(creo_root)
    info.version = ver
    info.major_version = major

    bin_dir = creo_root / "Parametric" / "bin"
    creo_exe = bin_dir / "parametric.exe"
    creo_psf = _select_psf_file(bin_dir, psf_name)
    ptk_inc = creo_root / "Common Files" / "protoolkit" / "includes"
    ptk_obj = creo_root / "Common Files" / "protoolkit" / "x86e_win64" / "obj"
    protk_unlock = creo_root / "Common Files" / "x86e_win64" / "obj" / "protk_unlock.exe"
    templates_dir = creo_root / "Common Files" / "templates"

    if creo_exe.is_file():
        info.creo_exe = str(creo_exe)
    else:
        info.issues.append(f"Missing parametric.exe under {bin_dir}")

    if creo_psf and creo_psf.is_file():
        info.creo_psf = str(creo_psf)
    else:
        info.issues.append(f"Missing Creo startup .psf file under {bin_dir}")

    required_libs = ("protk_dll_NU.lib", "ucore.lib", "udata.lib")
    inc_ok = (ptk_inc / "ProToolkit.h").is_file()
    libs_ok = all((ptk_obj / lib).is_file() for lib in required_libs)
    unlock_ok = protk_unlock.is_file()

    if inc_ok:
        info.ptk_inc = str(ptk_inc)
    else:
        info.issues.append(
            f"Missing Pro/TOOLKIT headers ({ptk_inc / 'ProToolkit.h'}). "
            "Run Creo reconfigure.exe and enable the 'Creo TOOLKIT' / 'API Toolkits' component."
        )

    if libs_ok:
        info.ptk_obj = str(ptk_obj)
    else:
        info.issues.append(
            f"Missing Pro/TOOLKIT x64 libraries under {ptk_obj}. "
            "Run Creo reconfigure.exe and enable 'Creo TOOLKIT'."
        )

    if unlock_ok:
        info.protk_unlock = str(protk_unlock)
    else:
        info.issues.append(f"Missing protk_unlock.exe at {protk_unlock}")

    if templates_dir.is_dir():
        info.templates_dir = str(templates_dir)
    else:
        info.issues.append(f"Missing Creo templates directory at {templates_dir}")

    # Resolve license: explicit -> env PTC_D_LICENSE_FILE -> env LM_LICENSE_FILE -> .psf file
    if explicit_license and explicit_license.strip():
        info.license_value = explicit_license.strip()
        info.license_source = "explicit"
    elif os.environ.get("PTC_D_LICENSE_FILE", "").strip():
        info.license_value = os.environ["PTC_D_LICENSE_FILE"].strip()
        info.license_source = "env:PTC_D_LICENSE_FILE"
    elif os.environ.get("LM_LICENSE_FILE", "").strip():
        info.license_value = os.environ["LM_LICENSE_FILE"].strip()
        info.license_source = "env:LM_LICENSE_FILE"
    elif creo_psf and creo_psf.is_file():
        psf_lic = parse_psf_license(creo_psf)
        if psf_lic:
            info.license_value = psf_lic
            info.license_source = f"psf:{creo_psf.name}"

    info.license_valid = is_valid_license_spec(info.license_value)
    if not info.license_valid:
        info.issues.append(
            "No valid PTC license found (checked CLI/config, env PTC_D_LICENSE_FILE/LM_LICENSE_FILE, and parametric.psf)."
        )

    info.protoolkit_sdk_ok = inc_ok and libs_ok and unlock_ok
    info.creo_ok = bool(info.creo_exe and info.creo_psf and info.templates_dir)
    return info


def _iter_registry_creo_candidates() -> list[Path]:
    if winreg is None:
        return []
    found: list[Path] = []
    hives = [winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER]
    subkeys = [
        r"SOFTWARE\PTC",
        r"SOFTWARE\WOW6432Node\PTC",
        r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
        r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
    ]
    for hive in hives:
        for base_sub in subkeys:
            try:
                with winreg.OpenKey(hive, base_sub) as key:
                    i = 0
                    while True:
                        try:
                            sub_name = winreg.EnumKey(key, i)
                            i += 1
                        except OSError:
                            break
                        try:
                            with winreg.OpenKey(key, sub_name) as sk:
                                for val_name in ("InstallLocation", "InstallationDir", "CREO_DIR", "DisplayIcon"):
                                    try:
                                        raw_val, _ = winreg.QueryValueEx(sk, val_name)
                                    except OSError:
                                        continue
                                    if not isinstance(raw_val, str) or "creo" not in raw_val.lower():
                                        continue
                                    cleaned = raw_val.strip().strip('"').split(",")[0].strip()
                                    p = Path(cleaned)
                                    if p.is_file() and p.name.lower() == "parametric.exe":
                                        found.append(p.parents[2])
                                    elif p.is_dir():
                                        if (p / "Parametric" / "bin" / "parametric.exe").is_file():
                                            found.append(p)
                                        elif (p.parent / "Parametric" / "bin" / "parametric.exe").is_file():
                                            found.append(p.parent)
                        except OSError:
                            continue
            except OSError:
                continue
    return found


def _existing_windows_drives() -> list[str]:
    ordered = ["C", "D", "E", "F"] + [c for c in string.ascii_uppercase if c not in ("A", "B", "C", "D", "E", "F")]
    drives: list[str] = []
    for letter in ordered:
        root = Path(f"{letter}:\\")
        if root.exists():
            drives.append(f"{letter}:\\")
    return drives


def find_creo_candidates(extra_roots: list[Path] | None = None) -> list[Path]:
    """Find candidate Creo Parametric installation directories across the machine."""
    raw: list[Path] = []
    if extra_roots:
        raw.extend(extra_roots)

    which_exe = shutil.which("parametric.exe")
    if which_exe:
        p = Path(which_exe).resolve()
        if len(p.parents) >= 3:
            raw.append(p.parents[2])

    raw.extend(_iter_registry_creo_candidates())

    for drive in _existing_windows_drives():
        dpath = Path(drive)
        for base in (dpath / "Program Files" / "PTC", dpath / "PTC", dpath):
            if not base.is_dir():
                continue
            try:
                for entry in base.iterdir():
                    if not entry.is_dir() or "creo" not in entry.name.lower():
                        continue
                    if (entry / "Parametric" / "bin" / "parametric.exe").is_file():
                        raw.append(entry)
                    else:
                        # Handle layouts like E:\Creo8.0\Creo 8.0.0.0
                        try:
                            for sub in entry.iterdir():
                                if sub.is_dir() and (sub / "Parametric" / "bin" / "parametric.exe").is_file():
                                    raw.append(sub)
                        except OSError:
                            pass
            except OSError:
                pass

    deduped: list[Path] = []
    seen: set[str] = set()
    for cand in raw:
        try:
            key = str(cand.resolve()).lower()
        except OSError:
            key = str(cand).lower()
        if key not in seen:
            seen.add(key)
            deduped.append(cand)
    return deduped


def discover_creo(
    *,
    explicit_root: str | Path | None = None,
    explicit_license: str | None = None,
    psf_name: str | None = None,
    config_path: Path | None = None,
) -> CreoInstallInfo:
    """Resolve the best Creo installation on this machine."""
    cfg = load_local_config(config_path)

    # 1. Explicit CLI arg -> env CREOBUILD_CREO_ROOT / CREO_ROOT -> creobuild.local.json
    direct_sources: list[tuple[str, Path]] = []
    if explicit_root:
        direct_sources.append(("cli", Path(explicit_root)))
    for env_key in ("CREOBUILD_CREO_ROOT", "CREO_ROOT"):
        val = os.environ.get(env_key, "").strip()
        if val:
            direct_sources.append((f"env:{env_key}", Path(val)))
    if cfg.get("creo_root"):
        direct_sources.append(("config", Path(str(cfg["creo_root"]))))

    lic_override = explicit_license or cfg.get("license_file")
    for src_name, root_path in direct_sources:
        if root_path.exists():
            return inspect_creo_installation(
                root_path, psf_name=psf_name or cfg.get("psf_name"), explicit_license=lic_override, source=src_name
            )
        info = CreoInstallInfo(creo_root=str(root_path), source=src_name)
        info.issues.append(f"Configured Creo root does not exist: {root_path}")
        return info

    # 2. Auto-scan system
    candidates = find_creo_candidates()
    if not candidates:
        info = CreoInstallInfo(source="auto")
        info.issues.append(
            "Could not auto-detect any Creo Parametric installation. "
            "Pass --creo-root '<Creo install folder>' or run python setup.py --creo-root '<path>'."
        )
        return info

    scored: list[tuple[tuple[int, int, int, int], CreoInstallInfo]] = []
    for cand in candidates:
        info = inspect_creo_installation(cand, psf_name=psf_name, explicit_license=lic_override, source="auto")
        ver_bonus = 1 if info.major_version == 8 else 0
        score = (
            1 if (info.creo_ok and info.protoolkit_sdk_ok and info.license_valid) else 0,
            1 if info.protoolkit_sdk_ok else 0,
            1 if info.creo_ok else 0,
            (info.major_version or 0) * 10 + ver_bonus,
        )
        scored.append((score, info))
    scored.sort(key=lambda item: item[0], reverse=True)
    return scored[0][1]


def _find_vswhere() -> Path | None:
    roots = [
        os.environ.get("ProgramFiles(x86)", ""),
        os.environ.get("ProgramFiles", ""),
    ]
    for base in roots:
        if not base:
            continue
        cand = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if cand.is_file():
            return cand
    return None


def _resolve_msvc_from_path_or_dir(cand: Path, source: str) -> MsvcToolchainInfo | None:
    if cand.is_file():
        name_low = cand.name.lower()
        if name_low == "vcvarsall.bat":
            return MsvcToolchainInfo(
                ok=True, kind="vs", setup_bat=str(cand), setup_args=["x64"], source=source
            )
        return MsvcToolchainInfo(ok=True, kind="portable" if "setup_" in name_low else "vs", setup_bat=str(cand), source=source)

    if cand.is_dir():
        for rel, args in (
            (Path("setup_x64.bat"), []),
            (Path("VC") / "Auxiliary" / "Build" / "vcvars64.bat", []),
            (Path("VC") / "Auxiliary" / "Build" / "vcvarsall.bat", ["x64"]),
        ):
            bat = cand / rel
            if bat.is_file():
                kind = "portable" if rel.name == "setup_x64.bat" else "vs"
                return MsvcToolchainInfo(ok=True, kind=kind, setup_bat=str(bat), setup_args=args, source=source)
    return None


def discover_msvc(
    *,
    explicit_setup: str | Path | None = None,
    config_path: Path | None = None,
    repo_root: Path | None = None,
) -> MsvcToolchainInfo:
    """Resolve an x64 MSVC C++17 compiler + Windows SDK environment."""
    rroot = repo_root or ROOT
    cfg = load_local_config(config_path)

    # 1. Explicit / env / saved config
    explicit_candidates: list[tuple[str, Path]] = []
    if explicit_setup:
        explicit_candidates.append(("cli", Path(explicit_setup)))
    if os.environ.get("CREOBUILD_MSVC_SETUP", "").strip():
        explicit_candidates.append(("env:CREOBUILD_MSVC_SETUP", Path(os.environ["CREOBUILD_MSVC_SETUP"].strip())))
    if cfg.get("msvc_setup"):
        explicit_candidates.append(("config", Path(str(cfg["msvc_setup"]))))

    for src, p in explicit_candidates:
        resolved = _resolve_msvc_from_path_or_dir(p, src)
        if resolved:
            return resolved
        return MsvcToolchainInfo(
            ok=False,
            source=src,
            issues=[f"Configured MSVC setup script/directory not found: {p}"],
        )

    # 2. Repo-local portable MSVC (.tools/portable_msvc/setup_x64.bat)
    local_portable = rroot / ".tools" / "portable_msvc" / "setup_x64.bat"
    if local_portable.is_file():
        return MsvcToolchainInfo(
            ok=True, kind="portable", setup_bat=str(local_portable), source="repo:.tools/portable_msvc"
        )

    # 3. Active developer prompt on PATH (only when INCLUDE and LIB are set)
    cl_on_path = shutil.which("cl.exe")
    link_on_path = shutil.which("link.exe")
    if cl_on_path and link_on_path and os.environ.get("INCLUDE") and os.environ.get("LIB"):
        return MsvcToolchainInfo(ok=True, kind="env", cl_exe=cl_on_path, source="env:PATH")

    # 4. Visual Studio Installer (vswhere.exe)
    vswhere = _find_vswhere()
    if vswhere is not None:
        try:
            res = subprocess.run(
                [
                    str(vswhere),
                    "-all",
                    "-products",
                    "*",
                    "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-latest",
                    "-property",
                    "installationPath",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
            )
            if res.returncode == 0:
                for line in res.stdout.splitlines():
                    inst = line.strip()
                    if not inst:
                        continue
                    resolved = _resolve_msvc_from_path_or_dir(Path(inst), "vswhere")
                    if resolved:
                        resolved.vs_install_path = inst
                        return resolved
        except OSError:
            pass

    # 5. Common Visual Studio & portable MSVC directories across drives
    for drive in _existing_windows_drives():
        dpath = Path(drive)
        probe_dirs = [
            dpath / "VSBuildTools" / "portable_msvc",
            dpath / "portable_msvc",
            dpath / "Program Files" / "Microsoft Visual Studio" / "2022" / "BuildTools",
            dpath / "Program Files" / "Microsoft Visual Studio" / "2022" / "Community",
            dpath / "Program Files" / "Microsoft Visual Studio" / "2022" / "Professional",
            dpath / "Program Files" / "Microsoft Visual Studio" / "2022" / "Enterprise",
            dpath / "Program Files (x86)" / "Microsoft Visual Studio" / "2019" / "BuildTools",
            dpath / "Program Files (x86)" / "Microsoft Visual Studio" / "2019" / "Community",
            dpath / "Program Files (x86)" / "Microsoft Visual Studio" / "2019" / "Professional",
            dpath / "Program Files (x86)" / "Microsoft Visual Studio" / "2019" / "Enterprise",
        ]
        for cand in probe_dirs:
            resolved = _resolve_msvc_from_path_or_dir(cand, "auto-scan")
            if resolved:
                return resolved

    return MsvcToolchainInfo(
        ok=False,
        source="auto",
        issues=[
            "No MSVC x64 C++ compiler / Windows SDK found. "
            "Run `python scripts/install_portable_msvc.py --accept-license` for a zero-UAC portable install, "
            "or install Visual Studio 2019/2022 C++ Build Tools."
        ],
    )


def capture_msvc_env(msvc: MsvcToolchainInfo) -> dict[str, str]:
    """Invoke setup_x64.bat / vcvars64.bat in cmd.exe and capture resulting env vars."""
    env = os.environ.copy()
    if msvc.kind == "env" or not msvc.setup_bat:
        return env

    bat = Path(msvc.setup_bat)
    if not bat.is_file():
        raise FileNotFoundError(f"MSVC setup script not found: {bat}")

    args_str = (" " + " ".join(msvc.setup_args)) if msvc.setup_args else ""
    cmd_str = f'cmd.exe /d /c "chcp 65001 >nul && call "{bat}"{args_str} >nul && set"'
    res = subprocess.run(
        cmd_str,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        timeout=60,
    )
    if res.returncode != 0:
        raise RuntimeError(f"Failed to initialize MSVC environment via {bat}:\n{res.stderr or res.stdout}")

    captured = dict(env)
    key_lookup = {existing_k.upper(): existing_k for existing_k in captured}
    for line in res.stdout.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if not k:
            continue
        norm_k = key_lookup.get(k.upper(), k.upper() if os.name == "nt" else k)
        captured[norm_k] = v
        key_lookup[k.upper()] = norm_k
    return captured


def collect_diagnostics(
    *,
    explicit_creo_root: str | Path | None = None,
    explicit_license: str | None = None,
    explicit_msvc_setup: str | Path | None = None,
    psf_name: str | None = None,
    config_path: Path | None = None,
    repo_root: Path | None = None,
) -> dict[str, Any]:
    """Gather structured diagnostics suitable for both humans and AI agents."""
    creo = discover_creo(
        explicit_root=explicit_creo_root,
        explicit_license=explicit_license,
        psf_name=psf_name,
        config_path=config_path,
    )
    msvc = discover_msvc(
        explicit_setup=explicit_msvc_setup,
        config_path=config_path,
        repo_root=repo_root,
    )

    ready = creo.creo_ok and creo.protoolkit_sdk_ok and creo.license_valid and msvc.ok
    next_steps: list[str] = []
    if not creo.creo_ok:
        next_steps.append("Install Creo Parametric (8.0+ recommended) or pass --creo-root '<Creo dir>' to setup.py.")
    elif not creo.protoolkit_sdk_ok:
        next_steps.append(
            "Run '<CREO_ROOT>/Parametric/bin/reconfigure.exe' (or PTC Installation Assistant) and check 'Creo TOOLKIT' / 'API Toolkits'."
        )
    if creo.creo_ok and not creo.license_valid:
        next_steps.append("Set PTC_D_LICENSE_FILE (e.g., 7788@host or path to license .dat) or pass --license to setup.py.")
    if not msvc.ok:
        next_steps.append(
            "Run `python scripts/install_portable_msvc.py --accept-license` (add `--mirror cn` in mainland China), "
            "or install VS 2019/2022 C++ Build Tools."
        )
    if ready:
        next_steps.append("Run `python build_cpp.py --plan plans/flanged_hub.json` to compile, unlock, and verify smoke test.")

    return {
        "ready": ready,
        "creo": asdict(creo),
        "msvc": asdict(msvc),
        "next_steps": next_steps,
    }
