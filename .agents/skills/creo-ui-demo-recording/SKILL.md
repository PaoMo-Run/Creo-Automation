---
name: creo-ui-demo-recording
description: >-
  Desktop UI automation & segmented HD screen recording workflow for Creo Parametric
  using scripts/human_ui.py and scripts/probe_creo_ui.py. Covers sandbox vs unlocked desktop
  boundaries, randomized C/S Bezier mouse physics & ~30% long-move overshoot pullback,
  Ctrl+Middle zoom (lateral lock against Z roll), Shift+Middle pan, Win11 MS Pinyin x Creo
  HCS16139P input safeguards, F2 global pause/resume HUD, and lossless FFmpeg concat MP4 output.
  Use when building or recording human-like Creo UI mechanism demos or debugging Win32/IME/FFmpeg.
---

# Creo Desktop UI Automation & Segmented Screen Recording Guide

This sub-skill documents the reusable Win32 UI automation and FFmpeg screen recording toolkit in [`scripts/human_ui.py`](../../../scripts/human_ui.py) and [`scripts/probe_creo_ui.py`](../../../scripts/probe_creo_ui.py).

---

## 1. Execution Boundaries (`exebox` vs. Unlocked Desktop)

| Capability | Headless / Restricted Agent Terminal (`exebox`) | Unlocked Physical Windows Desktop (`WinSta0\Default`) |
|---|---|---|
| `GetForegroundWindow()` | Returns `0` | Returns active Creo `HWND` |
| FFmpeg `gdigrab -i desktop` | Fails (`ERROR_ACCESS_DENIED`) | Captures `1920x1080` @ `30fps` H.264 with cursor |
| `SendInput` & Win11 MS Pinyin TSF | Does not reach user desktop / TSF | Drives real cursor curves, Pinyin composition & `F2` hotkey |
| Suitable tasks | `--test-mouse`, `probe_creo_ui.py --plan ...`, offline `PostMessageW` trail verification | `--test-ime`, `--dump-uia`, live MP4 recording |

---

## 2. Clean Un-Motored Stage & Default View Inheritance (`probe_creo_ui.py`)

1. Run `python scripts/probe_creo_ui.py --plan <asm_plan.json>` to strip top-level `motors`, `mechanism_analysis`, and `export_mech_end_view` while **preserving `default_view_camera`** cleanly into `build_cpp/record_stage/`.
2. Launch `parametric.exe parametric.psf <asm.1>` with `build_creo_process_env(creo)` without overriding `LANG`, keeping Creo's installed locale intact (`config.pro` disables datum/axis/point/csys display).

---

## 3. Humanized Mouse Physics & Creo Viewport Controls (`human_ui.py`)

### 3.1 Randomized C/S Curves & Stratified Overshoot (`plan_human_curve`)
1. **Multi-harmonic hand drift + asymmetric ease**: `plan_human_curve` / `eval_human_curve` build `10%–25%` C/S cubic-Bezier arcs with dual-harmonic `sin(pi*u)^1.2` lateral hand sway (endpoint error `< 1e-6 px`) and `_human_ease`.
2. **Stratified overshoot pullback**:
   - Long cross-pane moves (`dist >= 140px`, `precision=False`) trigger `15–38px` overshoot + `±11px` lateral jitter at `overshoot_prob=0.30` -> `45–95ms` pause -> smooth `0.14–0.25s` pullback.
   - Dense controls (table cells, `+`, radio buttons, floating name boxes, playback controls) pass **`precision=True`** for zero overshoot (`short_click_overshoot=0`).
3. Verify curve stats offline with `python scripts/human_ui.py --test-mouse`.

### 3.2 `Ctrl+Middle` Zoom, `Shift+Middle` Pan & Atomic Release
- **`Ctrl + Middle Mouse`** vertical drag: in Creo Parametric, **`dy > 0` (downward)** zooms **OUT** (panoramic) and `dy < 0` zooms IN. `ctrl_drag_middle` enforces `strict_vertical=True` (`±2.5px` lateral lock) so Creo never triggers Z-axis screen roll.
- **`Shift + Middle Mouse`** (`shift_drag_middle`) pans horizontally/vertically.
- `_drag_middle_with_mod` and `drag_left` wrap mouse buttons and modifier keys in `try...finally` + `PauseController` release guards so pausing/aborting never leaves `Ctrl`, `Shift`, or `MMB` stuck down.
- Reset to the assembly's saved isometric view before opening Playback using `send_ctrl_key(0x44)` (`Ctrl+D`), drag floating dialogs (`drag_window_to`) out of the 3D viewport and re-read `GetWindowRect`, and script explicit multi-pass playback loops (`*ANIMATION_FRAME <last>`).

---

## 4. Win11 MS Pinyin × Creo `HCS16139P` & `F2` Segmented FFmpeg

1. **DPIv2 & popup stability**: `human_ui.py` enables Per-Monitor DPIv2 (`-4`) on import; pass popup `HWND` explicitly on dialog clicks and wait for `GetWindowRect` layout settling after toggling Mechanism `单调`.
2. **Never switch away from `0x0804`**: On Chinese Creo `HCS16139P` floating boxes, keep `0x0804` active (`type_ascii(..., switch_en=False)` uses `_vk_press` hardware digits `'0'–'9'` and `KEYEVENTF_UNICODE` punctuation so `.` stays `.` and digits never drop); add table rows one by one skipping `0` cells (`_is_zero_val`) and verify committed entries against `trail.txt`. Run `python scripts/human_ui.py --test-ime` on unlocked desktop before long captures.
3. **`PauseController` & `FFmpegRecorder`**:
   - `PauseController` (`F2` default via `RegisterHotKey`) protects short IME/dropdown actions inside `critical_section()`, pauses cleanly into an overlay HUD without stealing focus, and accounts for paused wall-clock seconds.
   - `FFmpegRecorder` looks up `FFMPEG_PATH` -> `PATH` -> `Tools/ffmpeg/bin/ffmpeg.exe` -> `WinGet`, writes `seg_XXX.mp4` segments via `gdigrab`, and merges losslessly with `ffmpeg -f concat -safe 0 -i concat.txt -c copy -movflags +faststart`.
