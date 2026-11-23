# Chinese Creo 8.0 Mechanism UI Automation & Segmented HD Screen Recording Guide

This reference complements [SKILL.md](../SKILL.md) and documents the generalized desktop UI automation and FFmpeg recording architecture built around [`scripts/human_ui.py`](../../../../scripts/human_ui.py) and [`scripts/probe_creo_ui.py`](../../../../scripts/probe_creo_ui.py).

---

## 1. Execution Boundaries (`exebox` Sandbox vs. Unlocked Desktop)

| Capability | Headless / Restricted Agent Terminal (`exebox`) | Unlocked Physical Windows Desktop (`WinSta0\Default`) |
|---|---|---|
| Foreground window (`GetForegroundWindow()`) | Always returns `0` | Returns the real active Creo main `HWND` |
| FFmpeg `gdigrab -i desktop` | Fails with `ERROR_ACCESS_DENIED (error 5)` | Captures real desktop + cursor (`1920x1080` @ `30fps` H.264) |
| `SendInput` & Win11 MS Pinyin TSF | Does not inject into the physical user desktop | Drives smooth Bezier cursor motion, Pinyin composition, and `F2` hotkey |
| Recommended usage | `build_cpp.py` verification, `probe_creo_ui.py --plan ...`, `PostMessageW`/`SendMessageTimeoutW` trail verification | `human_ui.py --test-ime`, live UIA probes (`--dump-uia`), and full screen recording |

---

## 2. Clean Un-Motored Stage & UIA Calibration (`probe_creo_ui.py`)

1. **Strip motors cleanly**: Run `python scripts/probe_creo_ui.py --plan <asm_plan.json>` to copy the assembly plan with `motors` removed and `mechanism_analysis = false`, build component dependencies if missing, and stage `<part_name>.asm.1` (or `.prt.1`) into `build_cpp/record_stage/` alongside a clean `config.pro` (`datum_display no`, `axis_display no`, `datum_point_display no`, `display_coord_sys no`, `save_texture_with_model yes`, `prompt_on_exit no`, `search_path .`).
2. **Preserve OS default locale env**: Launch `parametric.exe parametric.psf <asm.1>` with `build_creo_process_env(creo)` without overriding `LANG` so Creo matches the user's native installed UI language (`chinese_cn` / `usascii`).
3. **Per-Monitor DPIv2 & explicit `HWND` binding**:
   - `scripts/human_ui.py` calls `SetProcessDpiAwarenessContext(-4)` on import to align Win32 `SendInput` physical pixels 1:1 with `GetWindowRect` and UIAutomation bounding boxes.
   - When interacting with popups (e.g., `配置文件详情` / Profile Details), wait `~0.45–0.50s` and poll `GetWindowRect` stability after switching to **单调 (`Table` / Monotonic)** before clicking `+` (`add_rows`); always pass the popup `hw` explicitly to click handlers so clicks never fall through to the background 3D viewport.
4. **UI Joint Direction Mapping (`FLIP_DIR`)**:
   - Newly picked `.first_trans_axis` / `.first_rot_axis` references in Creo Mechanism UI default to `+q` (`PRO_E_MOTOR_FLIP_DIR = 1`). Any joint whose JSON plan uses `"flip_dir": false` (`-q`) must click **反向方向** (`flip`) in the servo motor dashboard.

---

## 3. Win11 MS Pinyin × Creo `HCS16139P` Safe Input Protocol

### 3.1 Why `HCS16139P` Floating Dialogs Break After Switching to `0x0409`

- On Windows systems where `en-US (0409)` is language #1 and `zh-Hans-CN MS Pinyin (0804)` is #2, `xtop.exe` and its custom-drawn childless floating `PH.FEATNAME` window (`cls='HCS16139P'`, `WS_EX_TOOLWINDOW`) start on `0x08040804`.
- Calling `ensure_input_language(0x04090409)` before naming motors often switches Win32 `HKL` without reactivating the Windows 11 TSF IME service when returning to `HCS16139P`, causing Pinyin keystrokes to output raw ASCII letters.

### 3.2 Safeguards Built Into `HumanDriver`

1. **Never switch away from `0x0804`**: `HumanDriver.type_ascii(text, switch_en=False)` keeps the active MS Pinyin layout intact.
2. **Hybrid hardware digits + Unicode punctuation (`send_unicode_text`)**:
   - Digits `'0'–'9'` use real virtual key presses (`_vk_press(ord(ch))`) so Creo combo boxes (`MRU1`) never drop leading digits under `KEYEVENTF_UNICODE` (avoiding `"10"` -> `"00"`).
   - Non-digit characters such as `.` and `-` use `KEYEVENTF_UNICODE` so active Chinese IME punctuation mode never turns `.` into `。`.
3. **Live Win32 probe & recovery strategies**:
   - Run `python scripts/human_ui.py --test-ime` to validate MS Pinyin composition (`calibrate_ime_on_temp_window`) before long UI runs.
   - `HumanDriver.apply_zh_mode` supports fallback activation (`direct`, `shift`, `ctrl_space`, `alt_shift`, `win_space`, `win_space_shift`, `msg_0050`, `msg_0050_shift`).
   - During table entry under `单调`, add one row at a time (`+`) and skip default `0` cells immediately (`_is_zero_val`); verify committed cell values and motor names against `trail.txt` (`~ Update motor_profile...`).
   - In offline `PostMessageW` verification, wait until `trail.txt` confirms `PH.FEATNAME == ""` before sending synchronous `SendMessageTimeoutW(WM_CHAR)`.

---

## 4. `PauseController` (`F2` Hotkey) & Lossless Segmented `FFmpegRecorder`

- **`PauseController` (`F2` default, supports `F2`/`F8`/`F9`/`F10`/`PAUSE`)**:
  - Registers a system hotkey via `RegisterHotKey` (with `GetAsyncKeyState` fallback) so pause keypresses never leak into Creo.
  - Protects short focus-sensitive actions (`type_zh_phrase`, dropdown selection) inside `with pause_ctrl.critical_section():`.
  - On pause during `drag_middle`, sends `MOUSEEVENTF_MIDDLEUP` immediately to prevent stuck viewport dragging, displays a `WS_EX_NOACTIVATE | WS_EX_TOPMOST` HUD indicator, and accumulates pause duration so wait timeouts never spuriously expire.
  - On resume, `refocus_window_cleanly(hwnd)` restores/maximizes Creo and re-attaches foreground input cleanly.
- **Lossless segmented recording (`FFmpegRecorder`)**:
  - Captures exact clamped even-dimension desktop bounds (`clamp_capture_rect`) via `gdigrab` + `-draw_mouse 1` into `seg_XXX.mp4`.
  - Pauses close the active segment (`q\n`); resumes start a new segment so paused time never enters the final video; stop merges all segments losslessly with `ffmpeg -f concat -safe 0 -i concat.txt -c copy -movflags +faststart` using relative segment filenames.
