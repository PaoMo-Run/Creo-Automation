#!/usr/bin/env python3
"""
Prepare an un-motored Creo stage in build_cpp/record_stage/ for any assembly JSON plan
and optionally probe live UIAutomation controls (Ribbon, Tree, Mechanism tabs) for UI recording.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

import build_cpp  # noqa: E402
from creobuild_env import discover_creo, discover_msvc  # noqa: E402


PREP_DIR = build_cpp.BUILD_DIR / "record_prep"
STAGE_DIR = build_cpp.BUILD_DIR / "record_stage"


def strip_motors_for_ui_stage(plan_data: dict[str, Any]) -> dict[str, Any]:
    """Return a copy of an assembly plan stripped of servo motors and mechanism analysis."""
    clean = dict(plan_data)
    clean.pop("motors", None)
    clean["mechanism_analysis"] = False
    clean["export_mech_end_view"] = False
    clean["hide_datums_in_views"] = False
    return clean


def _collect_stage_deps(
    plan_path: pathlib.Path,
    plan_data: dict[str, Any],
    creo,
    dll_path: pathlib.Path,
    timeout_s: float = 360.0,
) -> list[pathlib.Path]:
    dest_dir = build_cpp.resolve_plan_dest_dir(plan_data)
    comps_dir_name = plan_data.get("components_dir_name", "components")
    comps_base = (dest_dir / str(comps_dir_name)) if comps_dir_name else dest_dir
    deps: list[pathlib.Path] = []
    for rel_plan in plan_data.get("components_plans", []):
        comp_plan = pathlib.Path(rel_plan)
        if not comp_plan.is_absolute():
            cand = plan_path.parent / comp_plan
            comp_plan = cand if cand.is_file() else (ROOT / comp_plan)
        comp_plan = comp_plan.resolve()
        comp_data = json.loads(comp_plan.read_text(encoding="utf-8"))
        comp_name = str(comp_data["part_name"])
        comp_folder = str(comp_data.get("component_folder_name") or comp_name)
        comp_dir = comps_base / comp_folder
        cached_prt = comp_dir / f"{comp_name}.prt.1"
        if cached_prt.is_file():
            deps.append(cached_prt)
        else:
            deps.append(
                build_cpp.build_and_verify_single_plan(
                    creo,
                    dll_path,
                    comp_plan,
                    comp_dir,
                    "build:zh",
                    skip_reopen=False,
                    clean_dest=True,
                    timeout_s=timeout_s,
                )
            )
    return deps


def prepare_unmotored_stage(plan_path: pathlib.Path, timeout_s: float = 360.0) -> pathlib.Path:
    """Build a clean un-motored model into build_cpp/record_stage/ using auto-discovered Creo."""
    plan_path = plan_path.resolve()
    plan_data = json.loads(plan_path.read_text(encoding="utf-8"))
    part_name = str(plan_data["part_name"])
    doc_type = str(plan_data.get("doc_type", "part"))
    mdl_ext = "asm" if doc_type == "assembly" else "prt"

    creo = discover_creo()
    msvc = discover_msvc(repo_root=ROOT)
    if not creo.creo_ok or not creo.protoolkit_sdk_ok or not creo.license_valid:
        raise RuntimeError(f"Creo environment not ready: {creo.issues}")
    if not msvc.ok:
        raise RuntimeError(f"MSVC toolchain not ready: {msvc.issues}")

    dll_path = build_cpp.compile_and_unlock(creo, msvc)
    deps = _collect_stage_deps(plan_path, plan_data, creo, dll_path, timeout_s=timeout_s)
    nomotor_plan = strip_motors_for_ui_stage(plan_data)

    if PREP_DIR.exists():
        shutil.rmtree(PREP_DIR)
    PREP_DIR.mkdir(parents=True)
    tmp_plan = PREP_DIR / f"{part_name}_nomotor.json"
    tmp_plan.write_text(json.dumps(nomotor_plan, indent=2, ensure_ascii=False), encoding="utf-8")

    report = build_cpp.run_creo_phase(
        creo,
        PREP_DIR / "run",
        dll_path,
        tmp_plan,
        "build:zh",
        "build_report.txt",
        extra_files=deps if deps else None,
        timeout_s=timeout_s,
    )
    if "STATUS=OK" not in report:
        raise RuntimeError(f"Un-motored stage build failed:\n{report}")

    out_mdl = build_cpp.latest_mdl_iteration(PREP_DIR / "run", part_name, ext=mdl_ext)
    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    STAGE_DIR.mkdir(parents=True)
    for dep in deps:
        shutil.copy2(dep, STAGE_DIR / dep.name)
    staged_mdl = STAGE_DIR / f"{part_name}.{mdl_ext}.1"
    shutil.copy2(out_mdl, staged_mdl)
    (STAGE_DIR / "config.pro").write_text(
        "web_browser_homepage about:blank\n"
        "prompt_on_exit no\n"
        "save_objects all\n"
        "save_texture_with_model yes\n"
        "datum_display no\n"
        "axis_display no\n"
        "datum_point_display no\n"
        "display_coord_sys no\n"
        "search_path .\n",
        encoding="utf-8",
    )
    print(f"[stage] Prepared clean un-motored model at {staged_mdl}")
    return staged_mdl


def dump_uia(el: object, uia: object, depth: int = 0, max_depth: int = 5, lines: list[str] | None = None) -> list[str]:
    if lines is None:
        lines = []
    if depth > max_depth:
        return lines
    try:
        r = el.CurrentBoundingRectangle
        w = r.right - r.left
        h = r.bottom - r.top
        nm = (el.CurrentName or "").replace("\r", " ").replace("\n", " ")
        aid = el.CurrentAutomationId or ""
        cls = el.CurrentClassName or ""
        ct = el.CurrentControlType
        if (nm or aid) and w > 0 and h > 0:
            lines.append(
                f"{'  ' * depth}ct={ct} name='{nm}' aid='{aid}' cls='{cls}' rect=({r.left},{r.top},{r.right},{r.bottom})"
            )
    except Exception:
        return lines
    if depth == max_depth:
        return lines
    walker = uia.ControlViewWalker
    child = walker.GetFirstChildElement(el)
    cnt = 0
    while child and cnt < 120:
        dump_uia(child, uia, depth + 1, max_depth, lines)
        child = walker.GetNextSiblingElement(child)
        cnt += 1
    return lines


def probe_live_creo(mdl_path: pathlib.Path, click_mech_tab: bool = True) -> None:
    from human_ui import HumanDriver, UIADriver  # noqa: E402

    creo = discover_creo()
    if not creo.creo_ok:
        raise RuntimeError(f"Creo installation not found: {creo.issues}")

    build_cpp.kill_creo()
    cmd = [creo.creo_exe, creo.creo_psf, mdl_path.name]
    proc_env = build_cpp.build_creo_process_env(creo)
    with open(mdl_path.parent / "probe_proc.log", "w", encoding="utf-8") as log_fh:
        proc = subprocess.Popen(cmd, cwd=str(mdl_path.parent), env=proc_env, stdout=log_fh, stderr=log_fh)
        try:
            driver = UIADriver()
            hwnd, win = driver.find_creo_window("Creo Parametric", timeout_s=60.0)
            time.sleep(4.0)
            rect = driver.maximize_and_focus(hwnd)
            time.sleep(1.2)
            lines = [f"WINDOW rect={rect}"]
            lines.extend(dump_uia(win, driver.uia, max_depth=5))
            (mdl_path.parent / "uia_dump_model.txt").write_text("\n".join(lines), encoding="utf-8")
            print(f"[probe] Wrote uia_dump_model.txt ({len(lines)} lines)")

            if click_mech_tab:
                hum = HumanDriver()
                apps_tabs = driver.find_descendants(win, name_contains="应用程序")
                if not apps_tabs:
                    apps_tabs = driver.find_descendants(win, name_contains="Applications")
                if apps_tabs:
                    cx, cy = driver.center_of(apps_tabs[0])
                    hum.click_at(cx, cy)
                    time.sleep(1.0)
                    mech_btns = driver.find_descendants(win, name_contains="机构")
                    if not mech_btns:
                        mech_btns = driver.find_descendants(win, name_contains="Mechanism")
                    for mb in mech_btns:
                        nm = (mb.CurrentName or "").strip()
                        if nm in ("机构", "Mechanism"):
                            mx, my = driver.center_of(mb)
                            hum.click_at(mx, my)
                            time.sleep(2.5)
                            break
                    mlines = dump_uia(win, driver.uia, max_depth=6)
                    (mdl_path.parent / "uia_dump_mech.txt").write_text("\n".join(mlines), encoding="utf-8")
                    print(f"[probe] Wrote uia_dump_mech.txt ({len(mlines)} lines)")
        finally:
            build_cpp.kill_creo()
            if proc.poll() is None:
                proc.terminate()


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare clean Creo UI stage and optionally dump UIA control tree")
    parser.add_argument("--plan", type=pathlib.Path, required=True, help="JSON modeling plan path")
    parser.add_argument("--dump-uia", action="store_true", help="Launch Creo UI and dump UIA controls")
    parser.add_argument("--timeout", type=float, default=360.0, help="Build timeout in seconds")
    args = parser.parse_args()

    staged = prepare_unmotored_stage(args.plan, timeout_s=args.timeout)
    if args.dump_uia:
        probe_live_creo(staged)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
