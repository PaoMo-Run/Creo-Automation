#!/usr/bin/env python3
"""Setup, environment diagnostics, skill installer, and onboarding wizard for Creo-Automation."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from creobuild_env import (
    LOCAL_CONFIG_NAME,
    ROOT,
    collect_diagnostics,
    save_local_config,
)

SKILL_SRC = ROOT / ".agents" / "skills" / "creo-parametric-automation"


def print_human_report(diag: dict) -> None:
    """Pretty-print environment health check for human users."""
    creo = diag["creo"]
    msvc = diag["msvc"]

    print("=== Creo-Automation Environment Check ===")
    print(f"Creo Root        : {creo['creo_root'] or '(not found)'} [source: {creo['source'] or '-'}]")
    if creo["version"]:
        ver_note = " (verified)" if creo["major_version"] == 8 else " (auto-adapted; community testing)"
        print(f"Creo Version     : {creo['version']}{ver_note}")
    print(f"Creo Executable  : {'OK' if creo['creo_ok'] else 'MISSING'}")
    print(f"Pro/TOOLKIT SDK  : {'OK' if creo['protoolkit_sdk_ok'] else 'MISSING'}")
    lic_status = "OK" if creo["license_valid"] else "MISSING/INVALID"
    print(f"PTC License      : {lic_status} ({creo['license_value'] or '-'} via {creo['license_source'] or '-'})")
    print(f"MSVC x64 C++17   : {'OK' if msvc['ok'] else 'MISSING'} ({msvc['kind'] or '-'} {msvc['setup_bat'] or msvc['cl_exe'] or ''})")

    all_issues = list(creo.get("issues", [])) + list(msvc.get("issues", []))
    if all_issues:
        print("\nDetected issues:")
        for issue in all_issues:
            print(f"  - {issue}")

    print("\nNext steps:")
    for step in diag.get("next_steps", []):
        print(f"  -> {step}")
    print(f"\nOverall status   : {'READY' if diag['ready'] else 'NEEDS SETUP'}")


def install_skill_globally(target: str) -> list[Path]:
    """Copy the repository Creo automation skill into user-level AI agent skill directories."""
    if not (SKILL_SRC / "SKILL.md").is_file():
        raise FileNotFoundError(f"Repository skill not found at {SKILL_SRC}")

    home = Path.home()
    targets_map = {
        "antigravity": home / ".gemini" / "config" / "skills" / "creo-parametric-automation",
        "claude": home / ".claude" / "skills" / "creo-parametric-automation",
        "codex": home / ".codex" / "skills" / "creo-parametric-automation",
    }
    selected = list(targets_map.values()) if target == "all" else [targets_map[target]]
    installed: list[Path] = []
    for dst in selected:
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(SKILL_SRC, dst)
        installed.append(dst)
    return installed


def persist_detected_config(diag: dict) -> Path | None:
    """Save valid discovered paths to creobuild.local.json."""
    creo = diag["creo"]
    msvc = diag["msvc"]
    payload: dict[str, str] = {}
    if creo.get("creo_root"):
        payload["creo_root"] = creo["creo_root"]
    if creo.get("license_value") and creo.get("license_source") == "explicit":
        payload["license_file"] = creo["license_value"]
    if msvc.get("setup_bat"):
        payload["msvc_setup"] = msvc["setup_bat"]
    if not payload:
        return None
    return save_local_config(payload, ROOT / LOCAL_CONFIG_NAME)


def run_portable_msvc_installer(mirror: str, install_dir: Path | None) -> int:
    """Invoke scripts/install_portable_msvc.py with accepted license."""
    cmd = [sys.executable, str(ROOT / "scripts" / "install_portable_msvc.py"), "--accept-license", "--mirror", mirror]
    if install_dir is not None:
        cmd.extend(["--dir", str(install_dir)])
    return subprocess.run(cmd, cwd=str(ROOT)).returncode


def run_smoke_test() -> int:
    """Build and verify the fast flanged_hub smoke-test model."""
    return subprocess.run(
        [sys.executable, str(ROOT / "build_cpp.py"), "--plan", str(ROOT / "plans" / "flanged_hub.json")],
        cwd=str(ROOT),
    ).returncode


def interactive_wizard(args: argparse.Namespace) -> int:
    """Guide beginners step-by-step through validating and deploying Creo-Automation."""
    diag = collect_diagnostics(
        explicit_creo_root=args.creo_root,
        explicit_license=args.license,
        explicit_msvc_setup=args.msvc_setup,
        psf_name=args.psf,
        repo_root=ROOT,
    )
    print_human_report(diag)

    creo = diag["creo"]
    if not creo["creo_ok"]:
        manual_root = input("\nEnter your Creo install folder (e.g. C:\\Program Files\\PTC\\Creo 8.0.0.0), or press Enter to skip: ").strip()
        if manual_root:
            args.creo_root = manual_root
            diag = collect_diagnostics(
                explicit_creo_root=args.creo_root,
                explicit_license=args.license,
                explicit_msvc_setup=args.msvc_setup,
                psf_name=args.psf,
                repo_root=ROOT,
            )
            print_human_report(diag)

    if not diag["creo"]["license_valid"] and diag["creo"]["creo_ok"]:
        manual_lic = input("\nEnter your PTC license path or port@host (e.g. 7788@localhost), or press Enter to skip: ").strip()
        if manual_lic:
            args.license = manual_lic
            diag = collect_diagnostics(
                explicit_creo_root=args.creo_root,
                explicit_license=args.license,
                explicit_msvc_setup=args.msvc_setup,
                psf_name=args.psf,
                repo_root=ROOT,
            )

    if not diag["msvc"]["ok"]:
        ans = input("\nMSVC x64 C++ compiler was not found. Install zero-UAC portable MSVC + WinSDK into .tools/portable_msvc now? [y/N]: ").strip().lower()
        if ans in ("y", "yes"):
            mirror = input("Download mirror ('global' for Microsoft/NuGet or 'cn' for Huawei NuGet mirror) [global]: ").strip().lower() or "global"
            if mirror not in ("global", "cn"):
                mirror = "global"
            rc = run_portable_msvc_installer(mirror, None)
            if rc != 0:
                return rc
            diag = collect_diagnostics(
                explicit_creo_root=args.creo_root,
                explicit_license=args.license,
                explicit_msvc_setup=args.msvc_setup,
                psf_name=args.psf,
                repo_root=ROOT,
            )
            print_human_report(diag)

    saved = persist_detected_config(diag)
    if saved:
        print(f"\nSaved local configuration to: {saved}")

    if diag["ready"]:
        run_now = input("\nEnvironment is READY! Run the fast 'flanged_hub' smoke test now? [Y/n]: ").strip().lower()
        if run_now in ("", "y", "yes"):
            return run_smoke_test()
    return 0 if diag["ready"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Setup and diagnose Creo-Automation environment")
    parser.add_argument("--check", action="store_true", help="Inspect environment without interactive prompts")
    parser.add_argument("--auto", action="store_true", help="Non-interactive auto-discovery & save creobuild.local.json")
    parser.add_argument("--json", action="store_true", help="Output structured JSON diagnostics")
    parser.add_argument("--strict", action="store_true", help="Return non-zero exit code when environment is not ready")
    parser.add_argument("--save", action="store_true", help="Save discovered/provided configuration to creobuild.local.json")
    parser.add_argument("--creo-root", type=str, default=None, help="Explicit Creo installation root")
    parser.add_argument("--license", type=str, default=None, help="Explicit PTC_D_LICENSE_FILE (.dat or port@host)")
    parser.add_argument("--msvc-setup", type=str, default=None, help="Explicit MSVC setup_x64.bat or vcvars64.bat")
    parser.add_argument("--psf", type=str, default=None, help="Explicit Creo startup .psf filename or path")
    parser.add_argument("--install-msvc", action="store_true", help="Install portable MSVC if no compiler is found")
    parser.add_argument("--mirror", choices=("global", "cn"), default="global", help="Mirror for portable MSVC WinSDK")
    parser.add_argument("--msvc-dir", type=Path, default=None, help="Custom directory for portable MSVC installation")
    parser.add_argument("--smoke-test", action="store_true", help="Run flanged_hub.json smoke test after setup")
    parser.add_argument(
        "--install-skill",
        choices=("antigravity", "claude", "codex", "all"),
        default=None,
        help="Install repo Agent Skill to user-level skill directory",
    )
    args = parser.parse_args()

    if args.install_skill:
        paths = install_skill_globally(args.install_skill)
        for p in paths:
            print(f"[skill] Installed creo-parametric-automation to: {p}")

    if not any((args.check, args.auto, args.save, args.install_msvc, args.smoke_test, args.install_skill)) and sys.stdin.isatty():
        return interactive_wizard(args)

    diag = collect_diagnostics(
        explicit_creo_root=args.creo_root,
        explicit_license=args.license,
        explicit_msvc_setup=args.msvc_setup,
        psf_name=args.psf,
        repo_root=ROOT,
    )

    if args.install_msvc and not diag["msvc"]["ok"]:
        rc = run_portable_msvc_installer(args.mirror, args.msvc_dir)
        if rc != 0:
            return rc
        diag = collect_diagnostics(
            explicit_creo_root=args.creo_root,
            explicit_license=args.license,
            explicit_msvc_setup=args.msvc_setup,
            psf_name=args.psf,
            repo_root=ROOT,
        )

    if args.save or args.auto:
        saved = persist_detected_config(diag)
        if saved and not args.json:
            print(f"[config] Saved to {saved}")

    if args.json:
        print(json.dumps(diag, indent=2, ensure_ascii=False))
    elif not args.install_skill or args.check or args.auto:
        print_human_report(diag)

    if args.smoke_test:
        if not diag["ready"]:
            print("ERROR: Cannot run smoke test until environment is ready.", file=sys.stderr)
            return 2
        return run_smoke_test()

    if args.strict and not diag["ready"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
