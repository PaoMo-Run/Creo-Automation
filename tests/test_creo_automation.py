#!/usr/bin/env python3
"""Unit tests for environment discovery, license parsing, trail checks, and JSON plans."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

import build_cpp
import creobuild_env
import human_ui
import install_portable_msvc
import probe_creo_ui
import setup as creobuild_setup


def _make_mock_creo_tree(root: Path, *, with_sdk: bool = True, psf_license: str = "7788@localhost") -> Path:
    bin_dir = root / "Parametric" / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    (bin_dir / "parametric.exe").write_bytes(b"MZ")
    (bin_dir / "parametric.psf").write_text(
        "// Commented license\n"
        "// ENV=PTC_D_LICENSE_FILE-=C:\\broken\\ignored.dat\n"
        f"ENV=PTC_D_LICENSE_FILE-={psf_license}\n",
        encoding="utf-8",
    )
    (root / "Common Files" / "templates").mkdir(parents=True, exist_ok=True)
    (root / "Common Files" / "templates" / "mmns_part_solid_abs.prt").write_bytes(b"TMPL")

    if with_sdk:
        inc = root / "Common Files" / "protoolkit" / "includes"
        obj = root / "Common Files" / "protoolkit" / "x86e_win64" / "obj"
        unlock_dir = root / "Common Files" / "x86e_win64" / "obj"
        inc.mkdir(parents=True, exist_ok=True)
        obj.mkdir(parents=True, exist_ok=True)
        unlock_dir.mkdir(parents=True, exist_ok=True)
        (inc / "ProToolkit.h").write_text("#pragma once\n", encoding="utf-8")
        for lib in ("protk_dll_NU.lib", "ucore.lib", "udata.lib"):
            (obj / lib).write_bytes(b"LIB")
        (unlock_dir / "protk_unlock.exe").write_bytes(b"MZ")
    return root


class TestCreoEnvDiscovery(unittest.TestCase):
    def test_parse_psf_license_variants(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            psf = Path(td) / "parametric.psf"
            psf.write_text(
                "// PTC PSF\n"
                "// ENV=PTC_D_LICENSE_FILE-=C:\\old\\bad.dat\n"
                "ENV=PTC_D_LICENSE_FILE-=\n"
                "ENV=LM_LICENSE_FILE=7788@backup-host\n"
                "ENV=PTC_D_LICENSE_FILE+=7788@primary-host\n",
                encoding="utf-8",
            )
            self.assertEqual(creobuild_env.parse_psf_license(psf), "7788@primary-host")

            psf2 = Path(td) / "lm_only.psf"
            psf2.write_text("ENV=LM_LICENSE_FILE=7788@lm-host\n", encoding="utf-8")
            self.assertEqual(creobuild_env.parse_psf_license(psf2), "7788@lm-host")

    def test_is_valid_license_spec(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dat = Path(td) / "license.dat"
            dat.write_text("SERVER host ANY 7788\n", encoding="utf-8")
            self.assertTrue(creobuild_env.is_valid_license_spec(str(dat)))
            self.assertTrue(creobuild_env.is_valid_license_spec("7788@flex-server"))
            self.assertFalse(creobuild_env.is_valid_license_spec(str(Path(td) / "missing.dat")))
            self.assertFalse(creobuild_env.is_valid_license_spec(""))

    def test_inspect_creo_complete_and_missing_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            creo_ok_dir = _make_mock_creo_tree(Path(td) / "Creo 10.0.2.0", with_sdk=True)
            info = creobuild_env.inspect_creo_installation(creo_ok_dir, source="test")
            self.assertTrue(info.creo_ok)
            self.assertTrue(info.protoolkit_sdk_ok)
            self.assertTrue(info.license_valid)
            self.assertEqual(info.major_version, 10)
            self.assertEqual(info.version, "10.0.2.0")

            creo_no_sdk = _make_mock_creo_tree(Path(td) / "Creo 8.0.0.0", with_sdk=False)
            info_bad = creobuild_env.inspect_creo_installation(creo_no_sdk, source="test")
            self.assertTrue(info_bad.creo_ok)
            self.assertFalse(info_bad.protoolkit_sdk_ok)
            self.assertTrue(any("reconfigure.exe" in msg for msg in info_bad.issues))

    def test_discover_msvc_portable_and_unicode_path(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = Path(td) / "测试 目录" / "Creo-Automation"
            portable = repo / ".tools" / "portable_msvc"
            portable.mkdir(parents=True, exist_ok=True)
            bat = portable / "setup_x64.bat"
            bat.write_text("@echo off\r\n", encoding="utf-8")

            msvc = creobuild_env.discover_msvc(repo_root=repo, config_path=repo / "none.json")
            self.assertTrue(msvc.ok)
            self.assertEqual(msvc.kind, "portable")
            self.assertEqual(Path(msvc.setup_bat), bat)

            vs_dir = Path(td) / "VS2022"
            (vs_dir / "VC" / "Auxiliary" / "Build").mkdir(parents=True, exist_ok=True)
            vcvars_all = vs_dir / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
            vcvars_all.write_text("@echo off\r\n", encoding="utf-8")
            msvc_vs = creobuild_env.discover_msvc(explicit_setup=vs_dir, config_path=repo / "none.json")
            self.assertTrue(msvc_vs.ok)
            self.assertEqual(msvc_vs.setup_args, ["x64"])

    def test_capture_msvc_env_unicode_and_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            bat_dir = Path(td) / "测试 空格目录"
            bat_dir.mkdir(parents=True, exist_ok=True)
            bat = bat_dir / "setup_x64.bat"
            bat.write_text(
                '@echo off\r\nset "CREOBUILD_TEST_VAR=hello_msvc_123"\r\nset "Path=C:\\mock_msvc;%Path%"\r\n',
                encoding="utf-8",
            )
            msvc = creobuild_env.MsvcToolchainInfo(ok=True, kind="portable", setup_bat=str(bat))
            env = creobuild_env.capture_msvc_env(msvc)
            self.assertEqual(env.get("CREOBUILD_TEST_VAR"), "hello_msvc_123")
            path_val = env.get("PATH") or env.get("Path") or ""
            self.assertTrue(path_val.startswith("C:\\mock_msvc;"))


class TestBuildHelpersAndPlans(unittest.TestCase):
    def test_find_trail_issues_bilingual(self) -> None:
        sample = (
            "!MT_CUST - Applied settings\n"
            "!%CW Sketch contains over-constrained geometry\n"
            "!%CE Feature regeneration failed\n"
            "OosEventHandler:: trail file out of sequence\n"
            "菜单操作 失序\n"
        )
        issues = build_cpp.find_trail_issues(sample)
        self.assertEqual(len(issues), 4)
        self.assertEqual(build_cpp.find_trail_issues("!MT_CUST - clean trail\n~ Close\n"), [])

    def test_bundled_plans_valid(self) -> None:
        for fname in ("flanged_hub.json", "turbofan_intake_fan.json"):
            plan_path = ROOT / "plans" / fname
            self.assertTrue(plan_path.is_file(), f"Missing plan: {plan_path}")
            data = json.loads(plan_path.read_text(encoding="utf-8"))
            self.assertIsInstance(data.get("part_name"), str)
            self.assertTrue(data["part_name"])
            self.assertIsInstance(data.get("operations"), list)
            self.assertGreater(len(data["operations"]), 0)

    def test_setup_check_json_cli(self) -> None:
        res = subprocess.run(
            [sys.executable, str(ROOT / "setup.py"), "--check", "--json"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=True,
        )
        payload = json.loads(res.stdout)
        self.assertIn("ready", payload)
        self.assertIn("creo", payload)
        self.assertIn("msvc", payload)
        self.assertIn("next_steps", payload)


class TestPortableMsvcManifestSelection(unittest.TestCase):
    def test_select_msvc_packages(self) -> None:
        manifest = {
            "packages": [
                {"id": "Microsoft.VC.14.29.16.11.Tools.HostX64.TargetX64.base", "payloads": [{"url": "u1"}]},
                {"id": "Microsoft.VC.14.29.16.11.Tools.HostX64.TargetX64.Res.base", "language": "zh-CN", "payloads": [{"url": "u2z"}]},
                {"id": "Microsoft.VC.14.29.16.11.Tools.HostX64.TargetX64.Res.base", "language": "en-US", "payloads": [{"url": "u2e"}]},
                {"id": "Microsoft.VC.14.29.16.11.CRT.Headers.base", "payloads": [{"url": "u3"}]},
                {"id": "Microsoft.VC.14.29.16.11.CRT.x64.Desktop.base", "payloads": [{"url": "u4"}]},
                {"id": "Microsoft.VC.14.29.16.11.CRT.x64.Store.base", "payloads": [{"url": "u5"}]},
            ]
        }
        ver, pkgs = install_portable_msvc.select_msvc_packages(manifest, preferred_prefix="14.29.16.11")
        self.assertEqual(ver, "14.29.16.11")
        self.assertEqual(len(pkgs), 5)
        self.assertEqual(pkgs[1]["language"], "en-US")

    def test_write_setup_x64_bat(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "VC" / "Tools" / "MSVC" / "14.29.30133").mkdir(parents=True)
            bat = install_portable_msvc.write_setup_x64_bat(root, "10.0.19041.0")
            txt = bat.read_text(encoding="utf-8")
            self.assertIn("%~dp0VC\\Tools\\MSVC\\14.29.30133", txt)
            self.assertIn("set \"SDK_VER=10.0.19041.0\"", txt)


class TestBuildRuntimeHelpers(unittest.TestCase):
    def test_build_creo_process_env_strips_stale_vars_and_prepends_runtime_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            creo_root = _make_mock_creo_tree(Path(td) / "Creo 8.0.0.0", with_sdk=True)
            for rel in (
                Path("Common Files") / "x86e_win64" / "lib",
                Path("Common Files") / "x86e_win64" / "deflib",
                Path("Common Files") / "libs" / "dfor" / "lib",
            ):
                (creo_root / rel).mkdir(parents=True, exist_ok=True)
            with mock.patch.dict(os.environ, {"PTC_D_LICENSE_FILE": "", "LM_LICENSE_FILE": ""}):
                info = creobuild_env.inspect_creo_installation(creo_root, source="test")
            base_env = {
                "PRO_DIRECTORY": "C:\\broken\\creo",
                "pro_machine_type": "x86e_win64",
                "CREO_COMMON_FILES": "C:\\broken\\common",
                "Path": "C:\\Windows\\System32",
            }
            env = build_cpp.build_creo_process_env(info, base_env=base_env)
            self.assertNotIn("PRO_DIRECTORY", {k.upper() for k in env})
            self.assertNotIn("PRO_MACHINE_TYPE", {k.upper() for k in env})
            self.assertNotIn("CREO_COMMON_FILES", {k.upper() for k in env})
            self.assertEqual(env.get("PTC_D_LICENSE_FILE"), "7788@localhost")
            path_parts = env["Path"].split(";")
            expected_prefix = [
                str(creo_root / "Common Files" / "x86e_win64" / "lib"),
                str(creo_root / "Common Files" / "x86e_win64" / "deflib"),
                str(creo_root / "Common Files" / "x86e_win64" / "obj"),
                str(creo_root / "Common Files" / "libs" / "dfor" / "lib"),
                str(creo_root / "Parametric" / "bin"),
            ]
            self.assertEqual(path_parts[:5], expected_prefix)
            self.assertEqual(path_parts[5], "C:\\Windows\\System32")

    def test_normalize_mirror_inf_utf8_and_gbk(self) -> None:
        build_block = (
            "   1 模型名称 PART\r\n"
            "   2 镜像项 已定义\r\n"
            "   3 镜像平面 RIGHT\r\n"
            "   4 不复制任何副本\r\n"
            "   5 完全从属副本\r\n"
        )
        reopen_block = (
            "   1 模型名称 PART\r\n"
            "   2 镜像平面 RIGHT\r\n"
            "   3 不复制任何副本\r\n"
            "   4 完全从属副本\r\n"
        )
        for enc in ("utf-8", "gbk"):
            norm = build_cpp.normalize_mirror_inf(build_block.encode(enc))
            self.assertEqual(norm, reopen_block.encode(enc))

        plain = b"FEATURE_COUNT 14\r\nVOL 100.0\r\n"
        self.assertEqual(build_cpp.normalize_mirror_inf(plain), plain)


class TestHumanUiAndStageHelpers(unittest.TestCase):
    def test_motion_curves_and_clamp_capture_rect(self) -> None:
        self.assertAlmostEqual(human_ui._min_jerk(0.0), 0.0)
        self.assertAlmostEqual(human_ui._min_jerk(0.5), 0.5)
        self.assertAlmostEqual(human_ui._min_jerk(1.0), 1.0)
        pt = human_ui._cubic_bezier((0.0, 0.0), (10.0, 0.0), (10.0, 20.0), (20.0, 20.0), 0.5)
        self.assertAlmostEqual(pt[0], 10.0)
        self.assertAlmostEqual(pt[1], 10.0)
        x, y, w, h = human_ui.clamp_capture_rect((-15, 7, 1925, 1086), (0, 0, 1920, 1080))
        self.assertEqual((x, y), (0, 7))
        self.assertEqual(w % 2, 0)
        self.assertEqual(h % 2, 0)
        self.assertEqual((w, h), (1920, 1072))

    def test_pause_controller_and_ffmpeg_cmd(self) -> None:
        with self.assertRaises(ValueError):
            human_ui.PauseController("F13")
        pc = human_ui.PauseController("F2", show_hud=False)
        self.assertFalse(pc.is_pause_requested())
        pc.toggle()
        self.assertTrue(pc.is_pause_requested())
        with pc.critical_section():
            self.assertEqual(pc.wait_if_paused(), 0.0)
        with tempfile.TemporaryDirectory() as td:
            out_mp4 = Path(td) / "demo.mp4"
            rec = human_ui.FFmpegRecorder(
                out_mp4,
                (0, 0, 1920, 1080),
                fps=30,
                ffmpeg_exe="ffmpeg",
                virtual_bounds=(0, 0, 1920, 1080),
            )
            cmd = rec.build_ffmpeg_command(Path(td) / "seg_000.mp4")
            self.assertIn("gdigrab", cmd)
            self.assertIn("libx264", cmd)
            self.assertIn("1920x1080", cmd)

    def test_strip_motors_for_ui_stage(self) -> None:
        raw = {
            "part_name": "demo_asm",
            "doc_type": "assembly",
            "default_view_camera": {"look_from": [0.72, 0.30, -0.69], "up": [0.0, 1.0, 0.0]},
            "motors": [{"name": "m1"}],
            "mechanism_analysis": True,
            "export_mech_end_view": True,
            "hide_datums_in_views": True,
        }
        cleaned = probe_creo_ui.strip_motors_for_ui_stage(raw)
        self.assertNotIn("motors", cleaned)
        self.assertFalse(cleaned["mechanism_analysis"])
        self.assertFalse(cleaned["export_mech_end_view"])
        self.assertFalse(cleaned["hide_datums_in_views"])
        self.assertEqual(cleaned["default_view_camera"], raw["default_view_camera"])
        self.assertIn("motors", raw)

    def test_human_curve_overshoot_and_mouse_self_test(self) -> None:
        rate, short_over, max_err = human_ui.run_mouse_self_test(
            seed=20261125,
            overshoot_prob=0.30,
            virtual_bounds=(0, 0, 1920, 1080),
        )
        self.assertEqual(short_over, 0)
        self.assertLess(max_err, 1e-6)
        self.assertGreaterEqual(rate, 0.18)
        self.assertLessEqual(rate, 0.45)

    def test_drawing_plan_detection_and_make_gb_dtl(self) -> None:
        dwg_plan = ROOT / "plans" / "dwg_flanged_hub.json"
        self.assertTrue(dwg_plan.is_file())
        data = json.loads(dwg_plan.read_text(encoding="utf-8"))
        self.assertTrue(build_cpp.is_drawing_plan(data))
        self.assertEqual(data["model_name"], "flanged_hub")
        with tempfile.TemporaryDirectory() as td:
            iso = Path(td) / "iso_mm.dtl"
            iso.write_text("projection_type THIRD_ANGLE\ndefault_font font\n", encoding="utf-8")
            gb = build_cpp.make_gb_dtl(Path(td), iso, font_name="chfntf")
            txt = gb.read_text(encoding="utf-8")
            self.assertIn("projection_type FIRST_ANGLE", txt)
            self.assertIn("default_font chfntf", txt)
            self.assertIn("symbol_font LEGACY", txt)

    def test_install_skill_globally_installs_all_skills(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            installed = creobuild_setup.install_skill_globally("antigravity", home=Path(td))
            names = sorted(p.name for p in installed)
            self.assertEqual(
                names,
                ["creo-engineering-drawing", "creo-parametric-automation", "creo-ui-demo-recording"],
            )
            for p in installed:
                self.assertTrue((p / "SKILL.md").is_file())


if __name__ == "__main__":
    unittest.main()
