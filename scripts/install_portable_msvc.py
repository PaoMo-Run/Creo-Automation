#!/usr/bin/env python3
"""Zero-UAC portable MSVC x64 + Windows 10 SDK installer for Creo-Automation.

Downloads official Microsoft Visual Studio MSVC toolset packages (.vsix) and
Windows SDK C++ NuGet packages (.nupkg) into `.tools/portable_msvc` (by default),
verifies SHA256 hashes for Visual Studio payloads, and generates `setup_x64.bat`.

Use `--accept-license` to acknowledge Microsoft's Visual Studio & Windows SDK
license terms: https://visualstudio.microsoft.com/license-terms/
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import re
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET_DIR = ROOT / ".tools" / "portable_msvc"
VS_CHANNEL_URL = "https://aka.ms/vs/17/release/channel"
NUGET_BASES = {
    "global": "https://api.nuget.org/v3-flatcontainer",
    "cn": "https://repo.huaweicloud.com/artifactory/api/nuget/v3/nuget-remote",
}
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Creo-Automation/1.0"
DEFAULT_MSVC_PREF = "14.29.16.11"
DEFAULT_SDK_PREF = "10.0.19041"


def _http_get_bytes(url: str, *, headers: dict[str, str] | None = None, timeout: float = 30.0) -> bytes:
    hdrs = {"User-Agent": USER_AGENT, "Accept-Encoding": "identity"}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, headers=hdrs)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def download_resumable(url: str, dest: Path, *, expected_sha256: str | None = None, expected_size: int | None = None) -> Path:
    """Download a file with HTTP Range resume and optional SHA256 verification."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_file() and expected_sha256:
        if file_sha256(dest).lower() == expected_sha256.lower():
            return dest
        dest.unlink()

    part_path = dest.with_suffix(dest.suffix + ".part")
    total_len: int | None = expected_size

    for attempt in range(25):
        current = part_path.stat().st_size if part_path.is_file() else 0
        if total_len is not None and current >= total_len > 0:
            break
        hdrs = {"User-Agent": USER_AGENT, "Accept-Encoding": "identity"}
        if current > 0:
            hdrs["Range"] = f"bytes={current}-"

        req = urllib.request.Request(url, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=30.0) as resp:
                if total_len is None:
                    cr = resp.headers.get("Content-Range")
                    if cr and "/" in cr:
                        try:
                            total_len = int(cr.rsplit("/", 1)[-1])
                        except ValueError:
                            total_len = None
                    elif resp.status == 200 and resp.headers.get("Content-Length"):
                        try:
                            total_len = int(resp.headers["Content-Length"])
                        except ValueError:
                            total_len = None

                mode = "ab" if (current > 0 and resp.status == 206) else "wb"
                if mode == "wb" and current > 0:
                    current = 0
                with open(part_path, mode) as fh:
                    while True:
                        try:
                            chunk = resp.read(524288)
                        except (OSError, urllib.error.URLError, http.client.HTTPException):
                            break
                        if not chunk:
                            break
                        fh.write(chunk)
                        current += len(chunk)
                if total_len is None or current >= total_len:
                    break
        except urllib.error.HTTPError as err:
            if err.code == 416 and part_path.is_file():
                break
            if attempt >= 24:
                raise
        except (OSError, urllib.error.URLError, http.client.HTTPException):
            if attempt >= 24:
                raise

    if expected_sha256:
        got = file_sha256(part_path).lower()
        if got != expected_sha256.lower():
            part_path.unlink(missing_ok=True)
            raise RuntimeError(f"SHA256 mismatch for {dest.name}: expected {expected_sha256}, got {got}")

    part_path.replace(dest)
    return dest


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1048576)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def select_msvc_packages(manifest: dict[str, Any], preferred_prefix: str = DEFAULT_MSVC_PREF) -> tuple[str, list[dict[str, Any]]]:
    """Select the 5 minimal x64 MSVC packages from a Visual Studio manifest."""
    packages = manifest.get("packages", [])
    by_id: dict[str, list[dict[str, Any]]] = {}
    versions: set[str] = set()

    ver_re = re.compile(r"^Microsoft\.VC\.(\d+\.\d+\.\d+\.\d+)\.Tools\.HostX64\.TargetX64\.base$")
    for pkg in packages:
        pid = pkg.get("id", "")
        by_id.setdefault(pid, []).append(pkg)
        m = ver_re.match(pid)
        if m:
            versions.add(m.group(1))

    if not versions:
        raise RuntimeError("No x64 MSVC toolset packages found in Visual Studio manifest.")

    matched = [v for v in sorted(versions) if v.startswith(preferred_prefix)]
    chosen_ver = matched[-1] if matched else sorted(versions)[-1]

    wanted = [
        (f"Microsoft.VC.{chosen_ver}.Tools.HostX64.TargetX64.base", None),
        (f"Microsoft.VC.{chosen_ver}.Tools.HostX64.TargetX64.Res.base", "en-US"),
        (f"Microsoft.VC.{chosen_ver}.CRT.Headers.base", None),
        (f"Microsoft.VC.{chosen_ver}.CRT.x64.Desktop.base", None),
        (f"Microsoft.VC.{chosen_ver}.CRT.x64.Store.base", None),
    ]

    selected: list[dict[str, Any]] = []
    for pkg_id, lang in wanted:
        cands = by_id.get(pkg_id, [])
        if lang:
            lang_cands = [p for p in cands if (p.get("language") or "").lower() == lang.lower()]
            cands = lang_cands or cands
        if not cands:
            raise RuntimeError(f"Required MSVC package missing from manifest: {pkg_id}")
        selected.append(cands[0])
    return chosen_ver, selected


def select_winsdk_version(nuget_base: str, preferred_prefix: str = DEFAULT_SDK_PREF) -> str:
    """Pick the latest stable Windows SDK version matching preferred_prefix from NuGet."""
    idx_url = f"{nuget_base.rstrip('/')}/microsoft.windows.sdk.cpp/index.json"
    data = json.loads(_http_get_bytes(idx_url).decode("utf-8"))
    stable = [v for v in data.get("versions", []) if "-" not in v]
    if not stable:
        raise RuntimeError(f"No stable microsoft.windows.sdk.cpp versions found at {idx_url}")
    matched = [v for v in stable if v.startswith(preferred_prefix)]
    return matched[-1] if matched else stable[-1]


def resolve_download_plan(
    *,
    mirror: str,
    msvc_prefix: str,
    sdk_prefix: str,
    cache_dir: Path,
) -> dict[str, Any]:
    """Fetch remote VS + NuGet metadata and compute the exact download plan."""
    ch = json.loads(_http_get_bytes(VS_CHANNEL_URL).decode("utf-8"))
    vsman_items = [x for x in ch.get("channelItems", []) if x.get("id") == "Microsoft.VisualStudio.Manifests.VisualStudio"]
    if not vsman_items:
        raise RuntimeError("Microsoft.VisualStudio.Manifests.VisualStudio missing from channel manifest.")
    vsman_payload = vsman_items[0]["payloads"][0]
    vsman_path = download_resumable(
        vsman_payload["url"],
        cache_dir / "VisualStudio.vsman",
    )
    manifest = json.loads(vsman_path.read_text(encoding="utf-8"))
    msvc_ver, msvc_pkgs = select_msvc_packages(manifest, preferred_prefix=msvc_prefix)

    nuget_base = NUGET_BASES[mirror].rstrip("/")
    sdk_nuget_ver = select_winsdk_version(nuget_base, preferred_prefix=sdk_prefix)
    sdk_pkgs = [
        {
            "id": "microsoft.windows.sdk.cpp",
            "version": sdk_nuget_ver,
            "url": f"{nuget_base}/microsoft.windows.sdk.cpp/{sdk_nuget_ver}/microsoft.windows.sdk.cpp.{sdk_nuget_ver}.nupkg",
            "filename": f"microsoft.windows.sdk.cpp.{sdk_nuget_ver}.nupkg",
        },
        {
            "id": "microsoft.windows.sdk.cpp.x64",
            "version": sdk_nuget_ver,
            "url": f"{nuget_base}/microsoft.windows.sdk.cpp.x64/{sdk_nuget_ver}/microsoft.windows.sdk.cpp.x64.{sdk_nuget_ver}.nupkg",
            "filename": f"microsoft.windows.sdk.cpp.x64.{sdk_nuget_ver}.nupkg",
        },
    ]
    return {
        "msvc_package_version": msvc_ver,
        "msvc_packages": msvc_pkgs,
        "sdk_nuget_version": sdk_nuget_ver,
        "sdk_packages": sdk_pkgs,
    }


def extract_vsix_to_root(vsix_path: Path, target_dir: Path) -> None:
    """Extract Contents/* from an MSVC .vsix archive into target_dir."""
    with zipfile.ZipFile(vsix_path, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if not name.startswith("Contents/"):
                continue
            rel = name[len("Contents/"):]
            if not rel:
                continue
            out_path = target_dir / rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info, "r") as src, open(out_path, "wb") as dst:
                shutil.copyfileobj(src, dst)


def extract_winsdk_nupkg(main_nupkg: Path, x64_nupkg: Path, target_dir: Path) -> str:
    """Extract Windows SDK headers, tools, and x64 libs, returning the installed SDK version folder."""
    kit_root = target_dir / "Windows Kits" / "10"
    with zipfile.ZipFile(main_nupkg, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if not name.startswith("c/"):
                continue
            rel = name[2:]
            out_path = kit_root / rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info, "r") as src, open(out_path, "wb") as dst:
                shutil.copyfileobj(src, dst)

    inc_dir = kit_root / "Include"
    sdk_vers = sorted(p.name for p in inc_dir.iterdir() if p.is_dir()) if inc_dir.is_dir() else []
    if not sdk_vers:
        raise RuntimeError(f"Could not detect extracted Windows SDK version under {inc_dir}")
    sdk_ver = sdk_vers[-1]

    with zipfile.ZipFile(x64_nupkg, "r") as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if name.startswith("c/ucrt/x64/"):
                rel = Path("Lib") / sdk_ver / "ucrt" / "x64" / name[len("c/ucrt/x64/"):]
            elif name.startswith("c/um/x64/"):
                rel = Path("Lib") / sdk_ver / "um" / "x64" / name[len("c/um/x64/"):]
            else:
                continue
            out_path = kit_root / rel
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info, "r") as src, open(out_path, "wb") as dst:
                shutil.copyfileobj(src, dst)
    return sdk_ver


def write_setup_x64_bat(target_dir: Path, sdk_ver: str) -> Path:
    """Write a relocatable setup_x64.bat using %~dp0 relative paths."""
    msvc_root = target_dir / "VC" / "Tools" / "MSVC"
    vers = sorted(p.name for p in msvc_root.iterdir() if p.is_dir()) if msvc_root.is_dir() else []
    if not vers:
        raise RuntimeError(f"Extracted MSVC directory not found under {msvc_root}")
    msvc_ver = vers[-1]

    bat_path = target_dir / "setup_x64.bat"
    bat_path.write_text(
        "@echo off\r\n"
        f'set "MSVC_ROOT=%~dp0VC\\Tools\\MSVC\\{msvc_ver}"\r\n'
        'set "SDK_ROOT=%~dp0Windows Kits\\10"\r\n'
        f'set "SDK_VER={sdk_ver}"\r\n'
        'set "PATH=%MSVC_ROOT%\\bin\\Hostx64\\x64;%SDK_ROOT%\\bin\\%SDK_VER%\\x64;%PATH%"\r\n'
        'set "INCLUDE=%MSVC_ROOT%\\include;%SDK_ROOT%\\Include\\%SDK_VER%\\ucrt;%SDK_ROOT%\\Include\\%SDK_VER%\\shared;%SDK_ROOT%\\Include\\%SDK_VER%\\um;%SDK_ROOT%\\Include\\%SDK_VER%\\winrt"\r\n'
        'set "LIB=%MSVC_ROOT%\\lib\\x64;%SDK_ROOT%\\Lib\\%SDK_VER%\\ucrt\\x64;%SDK_ROOT%\\Lib\\%SDK_VER%\\um\\x64"\r\n',
        encoding="utf-8",
    )
    return bat_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Install zero-UAC portable MSVC x64 + Windows SDK")
    parser.add_argument("--dir", type=Path, default=DEFAULT_TARGET_DIR, help="Destination directory")
    parser.add_argument("--mirror", choices=("global", "cn"), default="global", help="NuGet mirror for Windows SDK")
    parser.add_argument("--msvc-prefix", type=str, default=DEFAULT_MSVC_PREF, help="Preferred MSVC package prefix")
    parser.add_argument("--sdk-prefix", type=str, default=DEFAULT_SDK_PREF, help="Preferred Windows SDK prefix")
    parser.add_argument("--accept-license", action="store_true", help="Accept Microsoft Visual Studio & Windows SDK licenses")
    parser.add_argument("--keep-cache", action="store_true", help="Keep downloaded .vsix/.nupkg cache files")
    parser.add_argument("--dry-run", action="store_true", help="Resolve manifest and print download plan only")
    args = parser.parse_args()

    if not args.dry_run and not args.accept_license:
        print(
            "ERROR: Pass --accept-license to accept Microsoft Visual Studio and Windows SDK license terms:\n"
            "  https://visualstudio.microsoft.com/license-terms/",
            file=sys.stderr,
        )
        return 2

    target_dir = args.dir.resolve()
    cache_dir = target_dir / ".download_cache"
    print(f"[msvc] Resolving VS manifest and NuGet ({args.mirror})...")
    plan = resolve_download_plan(
        mirror=args.mirror,
        msvc_prefix=args.msvc_prefix,
        sdk_prefix=args.sdk_prefix,
        cache_dir=cache_dir,
    )

    if args.dry_run:
        summary = {
            "target_dir": str(target_dir),
            "msvc_package_version": plan["msvc_package_version"],
            "msvc_packages": [p["id"] for p in plan["msvc_packages"]],
            "sdk_nuget_version": plan["sdk_nuget_version"],
            "sdk_urls": [p["url"] for p in plan["sdk_packages"]],
        }
        if not args.keep_cache and (target_dir / ".download_cache").exists():
            shutil.rmtree(target_dir / ".download_cache")
            if target_dir.exists() and not any(target_dir.iterdir()):
                target_dir.rmdir()
        print(json.dumps(summary, indent=2))
        return 0

    vsix_files: list[Path] = []
    for pkg in plan["msvc_packages"]:
        for idx, pay in enumerate(pkg.get("payloads", [])):
            fname = Path(pay.get("fileName", f"{pkg['id']}_{idx}.vsix")).name
            out = cache_dir / fname
            print(f"[download] {pkg['id']} ({fname})...")
            download_resumable(
                pay["url"],
                out,
                expected_sha256=pay.get("sha256"),
            )
            vsix_files.append(out)

    sdk_files: list[Path] = []
    for spkg in plan["sdk_packages"]:
        out = cache_dir / spkg["filename"]
        print(f"[download] {spkg['id']} {spkg['version']}...")
        download_resumable(spkg["url"], out)
        sdk_files.append(out)

    print(f"[extract] Installing MSVC into {target_dir}...")
    for vf in vsix_files:
        extract_vsix_to_root(vf, target_dir)

    print(f"[extract] Installing Windows SDK into {target_dir}...")
    sdk_ver = extract_winsdk_nupkg(sdk_files[0], sdk_files[1], target_dir)
    bat = write_setup_x64_bat(target_dir, sdk_ver)

    if not args.keep_cache and cache_dir.exists():
        shutil.rmtree(cache_dir)

    print(f"[ok] Portable MSVC + Windows SDK ({sdk_ver}) installed cleanly.")
    print(f"[ok] Setup batch written to: {bat}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
