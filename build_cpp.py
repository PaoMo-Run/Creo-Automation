#!/usr/bin/env python3
"""Compile, unlock, execute, and verify Creo JSON-plan-driven C++ Pro/TOOLKIT models."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from creobuild_env import (
    CreoInstallInfo,
    MsvcToolchainInfo,
    capture_msvc_env,
    discover_creo,
    discover_msvc,
)

ROOT = Path(__file__).resolve().parent
SRC_CPP = ROOT / "src_c" / "creo_auto_modeler.cpp"
SRC_INC = ROOT / "src_c"
TEXT_DIR = ROOT / "src_c" / "text"
BUILD_DIR = ROOT / "build_cpp"
STAGE_DIR = BUILD_DIR / "stage"
VERIFY_DIR = BUILD_DIR / "verify"
OUT_DIR = ROOT / "output"
DEFAULT_PLAN = ROOT / "plans" / "flanged_hub.json"


def kill_creo() -> None:
    """Terminate any lingering Creo background/worker processes before or after a session."""
    for img in ("parametric.exe", "xtop.exe", "nmsd.exe"):
        res = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {img}"], capture_output=True, text=True)
        if img in res.stdout:
            subprocess.run(["taskkill", "/F", "/IM", img, "/T"], capture_output=True)
    for _ in range(30):
        res = subprocess.run(["tasklist", "/FI", "IMAGENAME eq xtop.exe"], capture_output=True, text=True)
        if "xtop.exe" not in res.stdout:
            break
        time.sleep(0.2)


def build_creo_process_env(creo: CreoInstallInfo, base_env: dict[str, str] | None = None) -> dict[str, str]:
    """Build a clean environment for parametric.exe without stale PRO_* overrides."""
    env = dict(base_env if base_env is not None else os.environ)
    for stale_key in ("PRO_DIRECTORY", "PRO_MACHINE_TYPE", "CREO_COMMON_FILES"):
        for existing in [k for k in env if k.upper() == stale_key]:
            env.pop(existing, None)

    creo_root = Path(creo.creo_root) if creo.creo_root else Path(creo.creo_exe).parents[2]
    runtime_dirs = [
        creo_root / "Common Files" / "x86e_win64" / "lib",
        creo_root / "Common Files" / "x86e_win64" / "deflib",
        creo_root / "Common Files" / "x86e_win64" / "obj",
        creo_root / "Common Files" / "libs" / "dfor" / "lib",
        creo_root / "Parametric" / "bin",
    ]
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    existing_parts = [p for p in env.get(path_key, "").split(os.pathsep) if p]
    existing_norm = {p.lower() for p in existing_parts}
    prefix_parts = [str(d) for d in runtime_dirs if d.is_dir() and str(d).lower() not in existing_norm]
    if prefix_parts:
        env[path_key] = os.pathsep.join(prefix_parts + existing_parts)

    if creo.license_value:
        env["PTC_D_LICENSE_FILE"] = creo.license_value
    return env


def compile_and_unlock(creo: CreoInstallInfo, msvc: MsvcToolchainInfo) -> Path:
    """Compile creobuild.dll using MSVC x64 and unlock it with Creo's protk_unlock.exe."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    obj_path = BUILD_DIR / "creobuild.obj"
    dll_path = BUILD_DIR / "creobuild.dll"
    for p in (obj_path, dll_path, BUILD_DIR / "creobuild.lib", BUILD_DIR / "creobuild.exp"):
        if p.exists():
            p.unlink()

    msvc_env = capture_msvc_env(msvc)
    msvc_path = msvc_env.get("PATH") or msvc_env.get("Path") or os.environ.get("PATH", "")
    cl_exe = shutil.which("cl.exe", path=msvc_path)
    link_exe = shutil.which("link.exe", path=msvc_path)
    if not cl_exe or not link_exe:
        raise FileNotFoundError(
            f"cl.exe or link.exe not found in initialized MSVC PATH (cl={cl_exe}, link={link_exe})"
        )

    ptk_inc = Path(creo.ptk_inc)
    ptk_obj = Path(creo.ptk_obj)
    ptk_libs = [str(ptk_obj / lib) for lib in ("protk_dll_NU.lib", "ucore.lib", "udata.lib")]
    win_libs = [
        "libcmt.lib",
        "kernel32.lib",
        "user32.lib",
        "wsock32.lib",
        "advapi32.lib",
        "mpr.lib",
        "winspool.lib",
        "netapi32.lib",
        "psapi.lib",
        "gdi32.lib",
        "shell32.lib",
        "comdlg32.lib",
        "ole32.lib",
        "ws2_32.lib",
    ]

    cl_cmd = [
        cl_exe,
        "-c",
        "/std:c++17",
        "/EHsc",
        "/bigobj",
        "/utf-8",
        "/W3",
        "/wd4828",
        "-D_CRT_SECURE_NO_WARNINGS",
        "-GS",
        "-fp:precise",
        "-D_WSTDIO_DEFINED",
        "-DPRO_MACHINE=36",
        "-DPRO_OS=4",
        f"-I{ptk_inc}",
        f"-I{SRC_INC}",
        f"-Fo{obj_path}",
        str(SRC_CPP),
    ]
    subprocess.run(cl_cmd, cwd=str(BUILD_DIR), env=msvc_env, check=True)

    link_cmd = [
        link_exe,
        f"-out:{dll_path}",
        "/subsystem:console",
        "/debug:none",
        "/machine:amd64",
        "/dll",
        str(obj_path),
        *ptk_libs,
        *win_libs,
    ]
    subprocess.run(link_cmd, cwd=str(BUILD_DIR), env=msvc_env, check=True)

    unlock_env = os.environ.copy()
    if creo.license_value:
        unlock_env["PTC_D_LICENSE_FILE"] = creo.license_value
    try:
        subprocess.run(
            [creo.protk_unlock, "-c", str(dll_path)],
            env=unlock_env,
            check=True,
        )
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"protk_unlock.exe failed (exit={exc.returncode}). "
            f"Verify your PTC license ({creo.license_value or 'unset'}) allows Pro/TOOLKIT unlocking."
        ) from exc
    return dll_path


def write_creo_runtime_files(
    workdir: Path,
    dll_path: Path,
    plan_path: Path,
    mode: str,
    templates_dir: Path,
    config_extra: str = "",
) -> None:
    """Create isolated creotk.dat, config.pro, text/, and plan files inside workdir."""
    shutil.copy2(dll_path, workdir / dll_path.name)
    shutil.copy2(plan_path, workdir / "creobuild_plan.json")
    if (workdir / "text").exists():
        shutil.rmtree(workdir / "text")
    shutil.copytree(TEXT_DIR, workdir / "text")
    (workdir / "creobuild_mode.txt").write_text(mode, encoding="utf-8")
    (workdir / "creotk.dat").write_text(
        f"""name       CreoBuildAutoModelerC
startup    dll
exec_file  ./{dll_path.name}
text_dir   ./text
allow_stop true
delay_start false
end
""",
        encoding="utf-8",
    )
    cfg_text = (
        "web_browser_homepage about:blank\n"
        "prompt_on_exit no\n"
        "save_objects all\n"
        "save_texture_with_model yes\n"
        "open_protk_unsigned_apps always\n"
        "search_path .\n"
        f"search_path {templates_dir}\n"
        + config_extra
    )
    (workdir / "config.pro").write_text(cfg_text, encoding="utf-8")


def find_trail_issues(trail_text: str) -> list[str]:
    """Return any Creo error, warning, or out-of-sequence lines from a trail log."""
    bad: list[str] = []
    for line in trail_text.splitlines():
        low = line.lower()
        if "!%ce" in low or "!%cw" in low or "失序" in line or "out of sequence" in low:
            bad.append(line)
    return bad


def run_creo_phase(
    creo: CreoInstallInfo,
    workdir: Path,
    dll_path: Path,
    plan_path: Path,
    mode: str,
    report_name: str,
    input_prt: Path | None = None,
    open_cli_prt: Path | None = None,
    cli_open_name: str | None = None,
    extra_files: list[Path] | None = None,
    stage_renames: list[tuple[Path, str]] | None = None,
    config_extra: str = "",
    timeout_s: float = 240.0,
) -> str:
    """Launch Creo in an isolated sandbox directory for build or reopen verification."""
    if workdir.exists():
        shutil.rmtree(workdir)
    workdir.mkdir(parents=True)
    if input_prt is not None:
        shutil.copy2(input_prt, workdir / input_prt.name)
    if extra_files:
        for ef in extra_files:
            shutil.copy2(ef, workdir / ef.name)
    if stage_renames:
        for src_p, dst_name in stage_renames:
            shutil.copy2(src_p, workdir / dst_name)
    if open_cli_prt is not None:
        shutil.copy2(open_cli_prt, workdir / f"{open_cli_prt.name}.1")

    write_creo_runtime_files(
        workdir,
        dll_path,
        plan_path,
        mode,
        Path(creo.templates_dir),
        config_extra=config_extra,
    )

    kill_creo()
    cmd = [creo.creo_exe, creo.creo_psf]
    if cli_open_name:
        cmd.append(cli_open_name)
    elif open_cli_prt is not None:
        cmd.append(f"{open_cli_prt.name}.1")

    proc_env = build_creo_process_env(creo)

    with open(workdir / "proc.log", "w", encoding="utf-8") as fh:
        subprocess.Popen(cmd, cwd=str(workdir), env=proc_env, stdout=fh, stderr=fh)

    report_path = workdir / report_name
    deadline = time.monotonic() + timeout_s
    report_text = ""
    while time.monotonic() < deadline:
        if report_path.exists():
            report_text = report_path.read_text(encoding="utf-8", errors="replace")
            if "STATUS=OK" in report_text or "STATUS=ERROR" in report_text:
                time.sleep(1.5)
                break
        time.sleep(0.4)

    kill_creo()
    if "STATUS=OK" not in report_text:
        trails = sorted(workdir.glob("trail.txt.*"))
        trail_tail = ""
        if trails:
            lines = trails[-1].read_text(encoding="utf-8", errors="replace").splitlines()
            trail_tail = "\n--- Trail tail ---\n" + "\n".join(lines[-25:])
        raise RuntimeError(f"Creo phase '{mode}' did not finish cleanly:\n{report_text}{trail_tail}")

    trails = sorted(workdir.glob("trail.txt.*"))
    if trails:
        trail_txt = trails[-1].read_text(encoding="utf-8", errors="replace")
        bad_lines = find_trail_issues(trail_txt)
        if bad_lines:
            raise RuntimeError(f"Creo trail reported errors/warnings in '{mode}':\n" + "\n".join(bad_lines))
    return report_text


def latest_mdl_iteration(folder: Path, stem: str, ext: str = "prt") -> Path:
    """Locate the highest-numbered <stem>.<ext>.<N> model iteration in folder."""
    candidates: list[tuple[int, Path]] = []
    for p in folder.iterdir():
        m = re.match(rf"^{re.escape(stem)}\.{re.escape(ext)}\.(\d+)$", p.name, re.IGNORECASE)
        if m:
            candidates.append((int(m.group(1)), p))
    if not candidates:
        raise FileNotFoundError(f"No {stem}.{ext}.<N> found in {folder}")
    candidates.sort(key=lambda item: item[0])
    return candidates[-1][1]


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def normalize_mirror_inf(raw: bytes) -> bytes:
    """Normalize Creo 8.0 build-time ProCmdFtMirror .inf blocks to match reopen exports."""
    for enc in ("utf-8", "gbk"):
        try:
            txt = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        if "镜像项" in txt:
            pat = re.compile(
                r"^[ \t]*2[ \t]+镜像项[ \t]+已定义[ \t]*\r?\n"
                r"([ \t]*)3([ \t]+镜像平面[^\r\n]*\r?\n)"
                r"([ \t]*)4([ \t]+不复制任何副本[^\r\n]*\r?\n)"
                r"([ \t]*)5([ \t]+完全从属副本[^\r\n]*\r?\n)",
                re.MULTILINE,
            )
            txt2, n = pat.subn(r"\g<1>2\g<2>\g<3>3\g<4>\g<5>4\g<6>", txt)
            if n > 0:
                return txt2.encode(enc)
    return raw


def collect_plan_textures(plan_data: dict, plan_dir: Path) -> list[Path]:
    """Find referenced texture bitmaps relative to plan_dir, plans/textures, or repo root."""
    seen: set[Path] = set()
    found: list[Path] = []
    candidates: list[str] = list(plan_data.get("textures", []))
    for op in plan_data.get("operations", []):
        tf = op.get("texture_file")
        if isinstance(tf, str) and tf:
            candidates.append(tf)
    for rel in candidates:
        p = Path(rel)
        if not p.is_absolute():
            for base in (plan_dir, plan_dir / "textures", ROOT / "plans" / "textures", ROOT):
                cand = base / p.name if (base / p.name).is_file() else (base / p)
                if cand.is_file():
                    p = cand
                    break
        p = p.resolve()
        if p.is_file() and p not in seen:
            seen.add(p)
            found.append(p)
    return found


def build_and_verify_single_plan(
    creo: CreoInstallInfo,
    dll_path: Path,
    plan_path: Path,
    dest_dir: Path,
    build_mode: str,
    skip_reopen: bool,
    extra_deps: list[Path] | None = None,
    clean_dest: bool = True,
    timeout_s: float = 240.0,
) -> Path:
    """Run build phase and independent reopen force-regen verification for one plan."""
    plan_data = json.loads(plan_path.read_text(encoding="utf-8"))
    part_name = str(plan_data["part_name"])
    doc_type = str(plan_data.get("doc_type", "part"))
    mdl_ext = "asm" if doc_type == "assembly" else "prt"

    tex_files = collect_plan_textures(plan_data, plan_path.parent)
    stage_extras = list(extra_deps or []) + tex_files

    needs_active_ui = (
        bool(plan_data.get("probe_mech"))
        or bool(plan_data.get("mechanism_analysis"))
        or bool(plan_data.get("motors"))
        or any(
            op.get("op") in ("solid_blend", "rotational_blend", "helical_sweep", "servo_motor", "mechanism_analysis")
            or (
                op.get("op") == "mirror"
                and (
                    "features" in op
                    or "feature" in op
                    or any(isinstance(r, dict) and "feature" in r for r in op.get("references", []))
                )
            )
            for op in plan_data.get("operations", [])
        )
    )
    cli_template = None
    if needs_active_ui:
        default_tmpl = "mmns_asm_design_abs" if doc_type == "assembly" else "mmns_part_solid_abs"
        tmpl_name = str(plan_data.get("template", default_tmpl))
        cli_template = Path(creo.templates_dir) / f"{tmpl_name}.{mdl_ext}"

    build_report = run_creo_phase(
        creo,
        STAGE_DIR,
        dll_path,
        plan_path,
        build_mode,
        "build_report.txt",
        open_cli_prt=cli_template,
        extra_files=stage_extras if stage_extras else None,
        timeout_s=timeout_s,
    )
    print(f"[build:{part_name}] Creo C++ Pro/TOOLKIT phase completed:")
    print(build_report.rstrip())

    if clean_dest and dest_dir.exists():
        shutil.rmtree(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)

    latest_mdl = latest_mdl_iteration(STAGE_DIR, part_name, ext=mdl_ext)
    out_mdl = dest_dir / f"{part_name}.{mdl_ext}.1"
    shutil.copy2(latest_mdl, out_mdl)
    for dep in (extra_deps or []):
        shutil.copy2(dep, dest_dir / dep.name)
    for fn in (
        f"{part_name}.stp",
        f"{part_name}.inf",
        f"{part_name}_default.jpg",
        f"{part_name}_front.jpg",
        f"{part_name}_top.jpg",
        f"{part_name}_right.jpg",
        f"{part_name}_mech_end.jpg",
        "build_report.txt",
    ):
        src = STAGE_DIR / fn
        if src.exists():
            shutil.copy2(src, dest_dir / fn)

    if not skip_reopen:
        verify_report = run_creo_phase(
            creo,
            VERIFY_DIR,
            dll_path,
            plan_path,
            "verify",
            "verify_result.txt",
            input_prt=out_mdl,
            cli_open_name=out_mdl.name if (doc_type == "assembly" and needs_active_ui) else None,
            extra_files=list(extra_deps) if extra_deps else None,
            timeout_s=timeout_s,
        )
        shutil.copy2(VERIFY_DIR / "verify_result.txt", dest_dir / "verify_result.txt")
        reopen_inf = VERIFY_DIR / "verify_reopen.inf"
        build_inf = dest_dir / f"{part_name}.inf"
        if reopen_inf.exists():
            shutil.copy2(reopen_inf, dest_dir / "verify_reopen.inf")
            if build_inf.exists():
                h1 = file_sha256(build_inf)
                h2 = file_sha256(dest_dir / "verify_reopen.inf")
                if h1 != h2:
                    nh1 = hashlib.sha256(normalize_mirror_inf(build_inf.read_bytes())).hexdigest().upper()
                    if nh1 != h2:
                        raise RuntimeError(f".inf SHA256 mismatch after reopen for '{part_name}': {h1} != {h2}")
                    print(f"[verify:{part_name}] .inf SHA256 match (normalized mirror rows): {nh1}")
                else:
                    print(f"[verify:{part_name}] .inf SHA256 match: {h1}")
        raw_open_jpg = VERIFY_DIR / "verify_open_raw.jpg"
        if raw_open_jpg.is_file():
            shutil.copy2(raw_open_jpg, dest_dir / f"{part_name}_verify_open_raw.jpg")
        print(f"[verify:{part_name}] Reopen & force-regen passed:")
        print(verify_report.rstrip())

    return out_mdl


def is_drawing_plan(plan_data: dict) -> bool:
    """Return True if plan JSON describes a 2D GB/T engineering drawing job."""
    return str(plan_data.get("doc_type", "")) == "drawing" or (
        "drawing_name" in plan_data and "model_name" in plan_data
    )


def make_gb_dtl(dest_dir: Path, iso_dtl_src: Path, font_name: str = "chfntf") -> Path:
    """Create a GB/T 4458.1 first-angle metric .dtl setup file from Creo's iso_mm.dtl."""
    base = iso_dtl_src.read_text(encoding="utf-8", errors="replace") if iso_dtl_src.is_file() else ""
    overrides = {
        "projection_type": "FIRST_ANGLE",
        "drawing_units": "MM",
        "decimal_marker": "PERIOD",
        "default_font": font_name,
        "default_annotation_font": font_name,
        "symbol_font": "LEGACY",
        "drawing_text_height": "3.500000",
        "def_view_text_height": "4.000000",
        "text_orientation": "PARALLEL_DIAM_HORIZ",
        "parallel_dim_placement": "ABOVE",
        "draw_arrow_style": "FILLED",
        "draw_arrow_length": "3.500000",
        "draw_arrow_width": "1.200000",
        "dim_text_gap": "1.000000",
        "dim_leader_length": "5.000000",
        "tol_display": "YES",
        "tol_text_height_factor": "0.650000",
        "tol_text_width_factor": "0.750000",
        "symmetric_tol_display_standard": "STD_ISO",
        "lead_trail_zeros": "STD_METRIC",
        "broken_view_offset": "5.000000",
        "view_scale_denominator": "10",
        "view_scale_format": "RATIO_COLON",
        "detail_view_boundary_type": "CIRCLE",
        "detail_view_circle": "ON",
        "detail_circle_line_style": "SOLIDFONT",
        "crossec_arrow_style": "HEAD_ONLINE",
        "crossec_arrow_length": "6.000000",
        "crossec_arrow_width": "2.000000",
        "default_show_2d_section_xhatch": "ASSEMBLY_AND_PART",
        "def_xhatch_break_around_text": "YES",
        "view_note": "STD_ISO",
    }
    lines: list[str] = []
    seen: set[str] = set()
    for raw in base.splitlines():
        parts = raw.strip().split()
        if parts and parts[0] in overrides:
            k = parts[0]
            lines.append(f"{k} {overrides[k]}")
            seen.add(k)
        else:
            lines.append(raw)
    for k, v in overrides.items():
        if k not in seen:
            lines.append(f"{k} {v}")
    out = dest_dir / "gb_mm.dtl"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out


def resolve_plan_dest_dir(plan_data: dict) -> Path:
    sub = plan_data.get("output_subdir")
    if isinstance(sub, str) and sub.strip():
        return OUT_DIR / sub.strip()
    if is_drawing_plan(plan_data):
        return OUT_DIR / str(plan_data["drawing_name"])
    part_name = str(plan_data["part_name"])
    return OUT_DIR / part_name


def build_and_verify_drawing_plan(
    creo: CreoInstallInfo,
    dll_path: Path,
    plan_path: Path,
    dest_dir: Path,
    build_mode: str,
    skip_reopen: bool,
    reuse_components: bool = False,
    render_pdf: bool = False,
    timeout_s: float = 360.0,
) -> Path:
    """Build a 2D GB/T engineering drawing (.drw.1 + .pdf + .dwg) and run isolated reopen verification."""
    plan_data = json.loads(plan_path.read_text(encoding="utf-8"))
    drw_name = str(plan_data["drawing_name"])
    model_name = str(plan_data["model_name"])
    tmpl_name = str(plan_data.get("template", "a3_drawing"))
    font_name = str(plan_data.get("font", "chfntf"))

    creo_root = Path(creo.creo_root) if creo.creo_root else Path(creo.creo_exe).parents[2]
    iso_tmpl_dir = creo_root / "Common Files" / "creo_standards" / "templates_iso_mm"
    iso_dtl_src = creo_root / "Common Files" / "creo_standards" / "draw_standards" / "iso_mm.dtl"
    tmpl_src = iso_tmpl_dir / f"{tmpl_name}.drw"
    if not tmpl_src.is_file():
        tmpl_src = Path(creo.templates_dir) / f"{tmpl_name}.drw"
    if not tmpl_src.is_file():
        raise FileNotFoundError(f"Drawing template '{tmpl_name}.drw' not found under {iso_tmpl_dir} or {creo.templates_dir}")

    # Resolve or build source 3D model (.prt.1)
    src_mdl_path: Path | None = None
    if plan_data.get("model_path"):
        cand = Path(str(plan_data["model_path"]))
        if not cand.is_absolute():
            cand = (ROOT / cand).resolve()
        if cand.is_file():
            src_mdl_path = cand
    if src_mdl_path is None:
        cached = resolve_plan_dest_dir({"part_name": model_name}) / f"{model_name}.prt.1"
        if reuse_components and cached.is_file():
            src_mdl_path = cached
    if src_mdl_path is None and plan_data.get("model_plan"):
        mplan = Path(str(plan_data["model_plan"]))
        if not mplan.is_absolute():
            cand = plan_path.parent / mplan
            mplan = cand if cand.is_file() else (ROOT / mplan)
        mplan = mplan.resolve()
        if not mplan.is_file():
            raise FileNotFoundError(f"Source model_plan not found: {mplan}")
        mdata = json.loads(mplan.read_text(encoding="utf-8"))
        m_dest = resolve_plan_dest_dir(mdata)
        print(f"[drawing] Building source 3D model '{model_name}' from {mplan.name}...")
        src_mdl_path = build_and_verify_single_plan(
            creo,
            dll_path,
            mplan,
            m_dest,
            build_mode,
            skip_reopen=skip_reopen,
            clean_dest=True,
            timeout_s=timeout_s,
        )
    if src_mdl_path is None:
        fallback = OUT_DIR / model_name / f"{model_name}.prt.1"
        if fallback.is_file():
            src_mdl_path = fallback
    if src_mdl_path is None or not src_mdl_path.is_file():
        raise FileNotFoundError(f"Source model '{model_name}.prt.1' not found; specify 'model_plan' or 'model_path'.")

    extras_dir = BUILD_DIR / f"_dwg_extras_{drw_name}"
    if extras_dir.exists():
        shutil.rmtree(extras_dir)
    extras_dir.mkdir(parents=True)
    gb_dtl = make_gb_dtl(extras_dir, iso_dtl_src, font_name=font_name)

    cfg_extra = (
        f"drawing_setup_file ./{gb_dtl.name}\n"
        f"search_path {iso_tmpl_dir}\n"
        "datum_display no\n"
        "axis_display no\n"
        "datum_point_display no\n"
        "display_coord_sys no\n"
        "pdf_use_pentable no\n"
    )
    build_report = run_creo_phase(
        creo,
        STAGE_DIR,
        dll_path,
        plan_path,
        "drawing",
        "build_report.txt",
        cli_open_name=f"{drw_name}.drw.1",
        stage_renames=[
            (src_mdl_path, f"{model_name}.prt.1"),
            (tmpl_src, f"{drw_name}.drw.1"),
            (gb_dtl, gb_dtl.name),
        ],
        config_extra=cfg_extra,
        timeout_s=timeout_s,
    )
    print(f"[drawing:{drw_name}] Creo C++ Pro/TOOLKIT drawing phase completed:")
    print(build_report.rstrip())

    if dest_dir.exists():
        shutil.rmtree(dest_dir)
    dest_dir.mkdir(parents=True)

    drw_latest = latest_mdl_iteration(STAGE_DIR, drw_name, ext="drw")
    prt_latest = latest_mdl_iteration(STAGE_DIR, model_name, ext="prt")
    out_drw = dest_dir / f"{drw_name}.drw.1"
    out_prt = dest_dir / f"{model_name}.prt.1"
    shutil.copy2(drw_latest, out_drw)
    shutil.copy2(prt_latest, out_prt)
    for fn in (f"{drw_name}.pdf", f"{drw_name}.dwg", "build_report.txt"):
        src = STAGE_DIR / fn
        if src.is_file():
            shutil.copy2(src, dest_dir / fn)

    pdf_out = dest_dir / f"{drw_name}.pdf"
    if render_pdf and pdf_out.is_file():
        try:
            import fitz  # Optional PyMuPDF
        except ImportError:
            print("[render] Optional PyMuPDF (`fitz`) not installed; skipping PNG preview render.")
        else:
            doc = fitz.open(str(pdf_out))
            page = doc[0]
            pix = page.get_pixmap(matrix=fitz.Matrix(220 / 72.0, 220 / 72.0), alpha=False)
            png_out = dest_dir / f"{drw_name}_preview.png"
            pix.save(str(png_out))
            doc.close()
            print(f"[render] Saved 220-DPI preview PNG: {png_out}")

    if not skip_reopen:
        verify_report = run_creo_phase(
            creo,
            VERIFY_DIR,
            dll_path,
            plan_path,
            "verify_drawing",
            "verify_result.txt",
            cli_open_name=f"{drw_name}.drw.1",
            stage_renames=[
                (out_prt, f"{model_name}.prt.1"),
                (out_drw, f"{drw_name}.drw.1"),
            ],
            timeout_s=timeout_s,
        )
        shutil.copy2(VERIFY_DIR / "verify_result.txt", dest_dir / "verify_result.txt")
        if "DRW_REOPEN_VERIFIED=true" not in verify_report:
            raise RuntimeError(f"Drawing reopen verification failed for '{drw_name}':\n{verify_report}")
        print(f"[verify_drawing:{drw_name}] Reopen & sheet-regen passed:")
        print(verify_report.rstrip())

    return out_drw


def export_clean_deliverable(plan_path: Path, src_dir: Path, deliverable_dir: Path) -> None:
    """Export a flat deliverable package containing only CAD binaries and preview JPGs."""
    plan_data = json.loads(plan_path.read_text(encoding="utf-8"))
    part_name = str(plan_data["part_name"])
    doc_type = str(plan_data.get("doc_type", "part"))
    if deliverable_dir.exists():
        shutil.rmtree(deliverable_dir)
    deliverable_dir.mkdir(parents=True)
    if doc_type == "assembly":
        asm_src = src_dir / f"{part_name}.asm.1"
        if asm_src.is_file():
            shutil.copy2(asm_src, deliverable_dir / asm_src.name)
        for prt in sorted(src_dir.glob("*.prt.1")):
            shutil.copy2(prt, deliverable_dir / prt.name)
    else:
        prt_src = src_dir / f"{part_name}.prt.1"
        if prt_src.is_file():
            shutil.copy2(prt_src, deliverable_dir / prt_src.name)
    for jpg in sorted(src_dir.glob("*.jpg")):
        shutil.copy2(jpg, deliverable_dir / jpg.name)
    print(f"[deliverable] Flat deliverable package written to: {deliverable_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and verify Creo JSON-plan-driven C++ modeler")
    parser.add_argument("--plan", type=Path, default=DEFAULT_PLAN, help="Path to modeling JSON plan")
    parser.add_argument("--creo-root", type=str, default=None, help="Override Creo installation root path")
    parser.add_argument("--license", type=str, default=None, help="Override PTC_D_LICENSE_FILE (.dat or port@host)")
    parser.add_argument("--msvc-setup", type=str, default=None, help="Override MSVC setup_x64.bat or vcvars64.bat path")
    parser.add_argument("--psf", type=str, default=None, help="Override Creo startup .psf filename or path")
    parser.add_argument("--skip-reopen", action="store_true", help="Skip isolated Creo reopen verification")
    parser.add_argument(
        "--default-names",
        action="store_true",
        help="Use Creo localized default feature names instead of custom feature names",
    )
    parser.add_argument(
        "--reuse-components",
        action="store_true",
        help="Reuse already built component .prt.1 files in output/<asm>/components when building assembly",
    )
    parser.add_argument(
        "--deliverable-dir",
        type=Path,
        default=None,
        help="Optional flat deliverable directory containing only .asm.1, .prt.1, and .jpg screenshots",
    )
    parser.add_argument("--render-pdf", action="store_true", help="Render 220-DPI PNG preview for drawing plans if PyMuPDF is installed")
    parser.add_argument("--timeout", type=float, default=360.0, help="Timeout per Creo phase in seconds")
    args = parser.parse_args()

    creo = discover_creo(
        explicit_root=args.creo_root,
        explicit_license=args.license,
        psf_name=args.psf,
    )
    msvc = discover_msvc(explicit_setup=args.msvc_setup, repo_root=ROOT)

    if not creo.creo_ok or not creo.protoolkit_sdk_ok or not creo.license_valid:
        print("ERROR: Creo environment check failed:", file=sys.stderr)
        for issue in creo.issues:
            print(f"  - {issue}", file=sys.stderr)
        print("Run `python setup.py --check` for full diagnostics and remediation.", file=sys.stderr)
        return 2

    if not msvc.ok:
        print("ERROR: MSVC C++17 x64 compiler not found:", file=sys.stderr)
        for issue in msvc.issues:
            print(f"  - {issue}", file=sys.stderr)
        print("Run `python scripts/install_portable_msvc.py --accept-license` or `python setup.py`.", file=sys.stderr)
        return 2

    plan_path = args.plan.resolve()
    if not plan_path.is_file():
        print(f"ERROR: Plan JSON not found: {plan_path}", file=sys.stderr)
        return 2
    if not SRC_CPP.is_file():
        print(f"ERROR: Missing C++ source: {SRC_CPP}", file=sys.stderr)
        return 2

    print(
        f"[env] Creo {creo.version or 'unknown'} ({creo.creo_root}) | "
        f"License ({creo.license_source}) | MSVC ({msvc.kind}: {msvc.source})"
    )

    plan_data = json.loads(plan_path.read_text(encoding="utf-8"))
    dll_path = compile_and_unlock(creo, msvc)
    print(f"[1/3] Compiled and unlocked: {dll_path}")

    build_mode = "build:default" if args.default_names else "build:zh"
    dest_dir = resolve_plan_dest_dir(plan_data)

    if is_drawing_plan(plan_data):
        build_and_verify_drawing_plan(
            creo,
            dll_path,
            plan_path,
            dest_dir,
            build_mode,
            args.skip_reopen,
            reuse_components=args.reuse_components,
            render_pdf=args.render_pdf,
            timeout_s=args.timeout,
        )
        print(f"[3/3] Clean drawing deliverables written to: {dest_dir}")
        return 0

    part_name = str(plan_data["part_name"])
    doc_type = str(plan_data.get("doc_type", "part"))
    comps_dir_name = plan_data.get("components_dir_name", "components")
    comps_base = (dest_dir / str(comps_dir_name)) if comps_dir_name else dest_dir

    dep_files: list[Path] = []
    if doc_type == "assembly":
        if not args.reuse_components and dest_dir.exists():
            shutil.rmtree(dest_dir)
        dest_dir.mkdir(parents=True, exist_ok=True)
        for rel_plan in plan_data.get("components_plans", []):
            comp_plan = Path(rel_plan)
            if not comp_plan.is_absolute():
                cand = plan_path.parent / comp_plan
                comp_plan = cand if cand.is_file() else (ROOT / comp_plan)
            comp_plan = comp_plan.resolve()
            if not comp_plan.is_file():
                print(f"ERROR: Component plan not found: {comp_plan}", file=sys.stderr)
                return 2
            comp_data = json.loads(comp_plan.read_text(encoding="utf-8"))
            comp_name = str(comp_data["part_name"])
            comp_folder = str(comp_data.get("component_folder_name") or comp_name)
            comp_dir = comps_base / comp_folder
            cached_prt = comp_dir / f"{comp_name}.prt.1"
            if args.reuse_components and cached_prt.is_file():
                print(f"[comp] Reusing existing '{comp_name}' from {cached_prt}")
                dep_files.append(cached_prt)
            else:
                print(f"[comp] Building component '{comp_name}' from {comp_plan.name}...")
                out_comp = build_and_verify_single_plan(
                    creo,
                    dll_path,
                    comp_plan,
                    comp_dir,
                    build_mode,
                    args.skip_reopen,
                    clean_dest=True,
                    timeout_s=args.timeout,
                )
                dep_files.append(out_comp)

    build_and_verify_single_plan(
        creo,
        dll_path,
        plan_path,
        dest_dir,
        build_mode,
        args.skip_reopen,
        extra_deps=dep_files if dep_files else None,
        clean_dest=(doc_type != "assembly"),
        timeout_s=args.timeout,
    )
    print(f"[3/3] Clean deliverables written to: {dest_dir}")

    if args.deliverable_dir is not None:
        export_clean_deliverable(plan_path, dest_dir, args.deliverable_dir.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
