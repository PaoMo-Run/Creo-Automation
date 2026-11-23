"""
Human-like desktop UI automation & FFmpeg screen-recording primitives for Windows + Creo.

Provides:
- Per-monitor DPI Awareness v2 coordinate handling (1:1 physical screen pixels)
- Cubic-Bezier smooth cursor motion with minimum-jerk velocity profile
- Smooth middle-mouse drag (viewport orbiting) and discrete wheel zooming
- Safe keyboard input on Chinese Windows 11 / Creo HCS16139P dialogs:
  * Hardware virtual-key digits ('0'-'9') + KEYEVENTF_UNICODE punctuation (avoids '.' -> '。'
    and '10' -> '00' drops without switching away from 0x0804 MS Pinyin)
  * Word-segmented MS Pinyin typing + multi-strategy IME activation + live Win32 EDIT probe
- Optional Windows UIAutomation discovery helper (`UIADriver`, lazy-imports `comtypes`)
- Global hotkey `PauseController` (`F2` default, `RegisterHotKey` + HUD + window refocus)
- Lossless segmented FFmpeg `gdigrab` MP4 recorder (`-f concat -safe 0 -c copy`)
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import math
import pathlib
import random
import shutil
import subprocess
import sys
import threading
import time
from typing import Any, Callable, Iterable, Optional

if sys.platform == "win32":
    import ctypes.wintypes as wt
    import winsound

    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except Exception:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            pass

    user32 = ctypes.windll.user32
    imm32 = ctypes.windll.imm32

    user32.GetForegroundWindow.restype = wt.HWND
    user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
    user32.GetWindowThreadProcessId.restype = wt.DWORD
    user32.GetKeyboardLayout.argtypes = [wt.DWORD]
    user32.GetKeyboardLayout.restype = ctypes.c_void_p
    user32.GetKeyboardLayoutList.argtypes = [wt.int if hasattr(wt, "int") else ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
    user32.GetKeyboardLayoutList.restype = ctypes.c_int
    user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_size_t]
    user32.PostMessageW.restype = wt.BOOL
    user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_size_t]
    user32.SendMessageW.restype = ctypes.c_ssize_t
    user32.SendMessageTimeoutW.argtypes = [
        wt.HWND,
        wt.UINT,
        ctypes.c_size_t,
        ctypes.c_size_t,
        wt.UINT,
        wt.UINT,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    user32.SendMessageTimeoutW.restype = ctypes.c_ssize_t
    user32.RegisterHotKey.argtypes = [wt.HWND, ctypes.c_int, wt.UINT, wt.UINT]
    user32.RegisterHotKey.restype = wt.BOOL
    user32.UnregisterHotKey.argtypes = [wt.HWND, ctypes.c_int]
    user32.UnregisterHotKey.restype = wt.BOOL
    user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
    user32.GetAsyncKeyState.restype = ctypes.c_short
    user32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
    user32.AttachThreadInput.restype = wt.BOOL
    user32.BringWindowToTop.argtypes = [wt.HWND]
    user32.BringWindowToTop.restype = wt.BOOL
    user32.IsIconic.argtypes = [wt.HWND]
    user32.IsIconic.restype = wt.BOOL
    user32.IsZoomed.argtypes = [wt.HWND]
    user32.IsZoomed.restype = wt.BOOL
    user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
    user32.ShowWindow.restype = wt.BOOL
    user32.SetForegroundWindow.argtypes = [wt.HWND]
    user32.SetForegroundWindow.restype = wt.BOOL
    imm32.ImmGetDefaultIMEWnd.argtypes = [wt.HWND]
    imm32.ImmGetDefaultIMEWnd.restype = wt.HWND

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [
            ("dx", wt.LONG),
            ("dy", wt.LONG),
            ("mouseData", wt.DWORD),
            ("dwFlags", wt.DWORD),
            ("time", wt.DWORD),
            ("dwExtraInfo", ctypes.c_size_t),
        ]

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [
            ("wVk", wt.WORD),
            ("wScan", wt.WORD),
            ("dwFlags", wt.DWORD),
            ("time", wt.DWORD),
            ("dwExtraInfo", ctypes.c_size_t),
        ]

    class HARDWAREINPUT(ctypes.Structure):
        _fields_ = [
            ("uMsg", wt.DWORD),
            ("wParamL", wt.WORD),
            ("wParamH", wt.WORD),
        ]

    class _INPUT_UNION(ctypes.Union):
        _fields_ = [
            ("mi", MOUSEINPUT),
            ("ki", KEYBDINPUT),
            ("hi", HARDWAREINPUT),
        ]

    class INPUT(ctypes.Structure):
        _fields_ = [
            ("type", wt.DWORD),
            ("union", _INPUT_UNION),
        ]
else:
    wt = None  # type: ignore[assignment]
    winsound = None  # type: ignore[assignment]
    user32 = None  # type: ignore[assignment]
    imm32 = None  # type: ignore[assignment]
    INPUT = Any  # type: ignore[misc,assignment]


INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_SCANCODE = 0x0008

WM_INPUTLANGCHANGEREQUEST = 0x0050
WM_IME_CONTROL = 0x0283
IMC_GETCONVERSIONMODE = 0x0001
IMC_SETCONVERSIONMODE = 0x0002
IMC_GETOPENSTATUS = 0x0005
IMC_SETOPENSTATUS = 0x0006
IME_CMODE_NATIVE = 0x0001

HKL_EN_US = 0x04090409
HKL_ZH_CN = 0x08040804

PAUSE_KEY_MAP: dict[str, int] = {
    "F2": 0x71,
    "F8": 0x77,
    "F9": 0x78,
    "F10": 0x79,
    "PAUSE": 0x13,
}


def _load_uia_module():
    """Lazy-load comtypes UIAutomationClient typelib so pure-stdlib callers don't require pip packages."""
    try:
        import comtypes
        import comtypes.client
    except ImportError as exc:
        raise RuntimeError(
            "UIADriver requires the optional 'comtypes' package (`pip install comtypes`)."
        ) from exc
    comtypes.client.GetModule("UIAutomationCore.dll")
    from comtypes.gen import UIAutomationClient as uia_mod  # noqa: E402

    return comtypes.client, uia_mod


def _require_win32() -> None:
    if sys.platform != "win32" or user32 is None:
        raise RuntimeError("Win32 GUI automation requires Windows x64.")


def _send_inputs(items: list[Any]) -> None:
    _require_win32()
    arr = (INPUT * len(items))(*items)
    user32.SendInput(len(items), arr, ctypes.sizeof(INPUT))


def get_cursor_pos() -> tuple[int, int]:
    _require_win32()
    pt = wt.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return int(pt.x), int(pt.y)


def _abs_mouse_coords(x: int, y: int) -> tuple[int, int]:
    _require_win32()
    v_left = user32.GetSystemMetrics(76)  # SM_XVIRTUALSCREEN
    v_top = user32.GetSystemMetrics(77)  # SM_YVIRTUALSCREEN
    v_w = max(1, user32.GetSystemMetrics(78))  # SM_CXVIRTUALSCREEN
    v_h = max(1, user32.GetSystemMetrics(79))  # SM_CYVIRTUALSCREEN
    ax = int(round(((x - v_left) + 0.5) * 65535.0 / v_w))
    ay = int(round(((y - v_top) + 0.5) * 65535.0 / v_h))
    return max(0, min(65535, ax)), max(0, min(65535, ay))


def _mouse_event(flags: int, x: int = 0, y: int = 0, data: int = 0, move: bool = False) -> None:
    if move:
        ax, ay = _abs_mouse_coords(x, y)
        flags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
    else:
        ax, ay = 0, 0
    inp = INPUT(
        type=INPUT_MOUSE,
        union=_INPUT_UNION(
            mi=MOUSEINPUT(
                dx=ax,
                dy=ay,
                mouseData=ctypes.c_uint32(data).value,
                dwFlags=flags,
                time=0,
                dwExtraInfo=0,
            )
        ),
    )
    _send_inputs([inp])


def _min_jerk(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return t * t * t * (10.0 - 15.0 * t + 6.0 * t * t)


def _cubic_bezier(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    u: float,
) -> tuple[float, float]:
    iu = 1.0 - u
    b0 = iu * iu * iu
    b1 = 3.0 * iu * iu * u
    b2 = 3.0 * iu * u * u
    b3 = u * u * u
    return (
        b0 * p0[0] + b1 * p1[0] + b2 * p2[0] + b3 * p3[0],
        b0 * p0[1] + b1 * p1[1] + b2 * p2[1] + b3 * p3[1],
    )


def clamp_capture_rect(
    rect: tuple[int, int, int, int],
    virtual_bounds: tuple[int, int, int, int],
) -> tuple[int, int, int, int]:
    """Clamp window rect to virtual screen bounds and enforce even width/height for yuv420p."""
    v_left, v_top, v_w, v_h = virtual_bounds
    left, top, right, bottom = rect
    left = max(v_left, left)
    top = max(v_top, top)
    right = min(v_left + v_w, right)
    bottom = min(v_top + v_h, bottom)
    w = max(640, (right - left) & ~1)
    h = max(480, (bottom - top) & ~1)
    return left, top, w, h


def _beep_cue(*tones: tuple[int, int]) -> None:
    if winsound is None:
        return

    def run() -> None:
        for freq, ms in tones:
            try:
                winsound.Beep(freq, ms)
            except Exception:
                break

    threading.Thread(target=run, daemon=True).start()


def refocus_window_cleanly(hwnd: int) -> None:
    """Bring maximized Creo main window cleanly back to foreground after Pause/Resume."""
    _require_win32()
    if not hwnd or not user32.IsWindow(hwnd):
        return
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        time.sleep(0.18)
    if not user32.IsZoomed(hwnd):
        user32.ShowWindow(hwnd, 3)  # SW_MAXIMIZE
        time.sleep(0.15)
    fg = user32.GetForegroundWindow()
    if fg != hwnd:
        fg_tid = int(user32.GetWindowThreadProcessId(fg, None)) if fg else 0
        tgt_tid = int(user32.GetWindowThreadProcessId(hwnd, None))
        attached = False
        if fg_tid and tgt_tid and fg_tid != tgt_tid:
            attached = bool(user32.AttachThreadInput(fg_tid, tgt_tid, True))
        try:
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
        finally:
            if attached:
                user32.AttachThreadInput(fg_tid, tgt_tid, False)
    time.sleep(0.25)


class PauseController:
    """
    Global hotkey pause/resume controller (default: F2).
    Intercepts the hotkey via RegisterHotKey (with GetAsyncKeyState fallback)
    and coordinates atomic critical sections, HUD overlay, and pause duration accounting.
    """

    def __init__(
        self,
        hotkey_name: str = "F2",
        on_pause: Optional[Callable[[], None]] = None,
        on_resume: Optional[Callable[[], None]] = None,
        show_hud: bool = True,
    ) -> None:
        key_up = hotkey_name.upper().strip()
        if key_up not in PAUSE_KEY_MAP:
            raise ValueError(f"Unsupported pause key '{hotkey_name}'. Supported: {sorted(PAUSE_KEY_MAP)}")
        self.key_label = key_up
        self.hotkey_vk = PAUSE_KEY_MAP[key_up]
        self.on_pause = on_pause
        self.on_resume = on_resume
        self.show_hud = show_hud

        self._lock = threading.Lock()
        self._pause_requested = False
        self._paused = False
        self._critical_depth = 0
        self._last_toggle_t = 0.0
        self._stop_event = threading.Event()
        self._hotkey_thread: Optional[threading.Thread] = None
        self._hud_close = threading.Event()
        self._hud_done = threading.Event()
        self._hud_done.set()

    def start(self) -> None:
        _require_win32()
        if self._hotkey_thread is not None and self._hotkey_thread.is_alive():
            return
        self._stop_event.clear()
        ready = threading.Event()
        mode_info: dict[str, str] = {"mode": "unknown"}

        def listener() -> None:
            registered = bool(user32.RegisterHotKey(None, 1, 0x4000, self.hotkey_vk))
            if not registered:
                registered = bool(user32.RegisterHotKey(None, 1, 0, self.hotkey_vk))
            mode_info["mode"] = "RegisterHotKey" if registered else "GetAsyncKeyState"
            ready.set()
            try:
                msg = wt.MSG()
                prev_down = False
                while not self._stop_event.is_set():
                    if registered:
                        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                            if msg.message == 0x0312 and int(msg.wParam) == 1:  # WM_HOTKEY
                                self.toggle()
                        time.sleep(0.025)
                    else:
                        down = bool(user32.GetAsyncKeyState(self.hotkey_vk) & 0x8000)
                        if down and not prev_down:
                            self.toggle()
                        prev_down = down
                        time.sleep(0.03)
            finally:
                if registered:
                    user32.UnregisterHotKey(None, 1)

        self._hotkey_thread = threading.Thread(target=listener, daemon=True)
        self._hotkey_thread.start()
        ready.wait(timeout=2.0)
        print(
            f"[hotkey] Pause/Resume hotkey ready ({self.key_label}, mode={mode_info['mode']})."
        )

    def stop(self) -> None:
        self._stop_event.set()
        self._close_hud()
        with self._lock:
            self._pause_requested = False
        if self._hotkey_thread is not None:
            self._hotkey_thread.join(timeout=1.5)
            self._hotkey_thread = None

    def toggle(self) -> None:
        now = time.monotonic()
        with self._lock:
            if now - self._last_toggle_t < 0.35:
                return
            self._last_toggle_t = now
            self._pause_requested = not self._pause_requested
            req = self._pause_requested
        if req:
            print(f"\n[PAUSE] {self.key_label} requested; pausing after current atomic step...")
        else:
            print(f"\n[RESUME] {self.key_label} requested; resuming...")

    def is_pause_requested(self) -> bool:
        with self._lock:
            return self._pause_requested

    @contextlib.contextmanager
    def critical_section(self):
        with self._lock:
            self._critical_depth += 1
        try:
            yield
        finally:
            with self._lock:
                self._critical_depth = max(0, self._critical_depth - 1)

    def _open_hud(self) -> None:
        if not self.show_hud or user32 is None:
            return
        self._hud_close.clear()
        self._hud_done.clear()

        def hud_worker() -> None:
            hw = 0
            try:
                hinst = ctypes.windll.kernel32.GetModuleHandleW(None)
                sw = max(800, user32.GetSystemMetrics(0))
                w_box, h_box = 360, 44
                x_pos = max(20, sw - w_box - 24)
                y_pos = 18
                ex_style = 0x08000088  # WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
                style = 0x80800201  # WS_POPUP | WS_BORDER | SS_CENTER | SS_CENTERIMAGE
                hw = user32.CreateWindowExW(
                    ex_style,
                    "STATIC",
                    f"  Recording Paused  |  Press {self.key_label} to Resume  ",
                    style,
                    x_pos,
                    y_pos,
                    w_box,
                    h_box,
                    None,
                    None,
                    hinst,
                    None,
                )
                if hw:
                    user32.ShowWindow(hw, 4)  # SW_SHOWNOACTIVATE
                msg = wt.MSG()
                while not self._hud_close.is_set() and not self._stop_event.is_set():
                    if hw:
                        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                            user32.TranslateMessage(ctypes.byref(msg))
                            user32.DispatchMessageW(ctypes.byref(msg))
                    time.sleep(0.03)
            except Exception:
                pass
            finally:
                if hw and user32.IsWindow(hw):
                    user32.DestroyWindow(hw)
                self._hud_done.set()

        threading.Thread(target=hud_worker, daemon=True).start()

    def _close_hud(self) -> None:
        self._hud_close.set()
        self._hud_done.wait(timeout=0.5)

    def wait_if_paused(self) -> float:
        with self._lock:
            if not self._pause_requested or self._critical_depth > 0 or self._stop_event.is_set():
                return 0.0
            self._paused = True

        t_start = time.monotonic()
        _beep_cue((660, 110), (500, 140))
        if self.on_pause:
            try:
                self.on_pause()
            except Exception as exc:
                print(f"[warn] on_pause callback error: {exc}")
        self._open_hud()
        try:
            while not self._stop_event.is_set():
                with self._lock:
                    if not self._pause_requested:
                        break
                time.sleep(0.05)
        finally:
            self._close_hud()
            if not self._stop_event.is_set() and self.on_resume:
                try:
                    self.on_resume()
                except Exception as exc:
                    print(f"[warn] on_resume callback error: {exc}")
            with self._lock:
                self._paused = False
            _beep_cue((520, 100), (780, 130))
        paused_s = time.monotonic() - t_start
        print(f"[RESUME] Resumed after {paused_s:.1f}s pause.")
        return paused_s

    def sleep(self, seconds: float) -> float:
        """Sleep `seconds` of active time, returning extra wall-clock seconds spent paused."""
        if seconds <= 0:
            return self.wait_if_paused()
        paused_total = 0.0
        rem = seconds
        while rem > 0 and not self._stop_event.is_set():
            paused_total += self.wait_if_paused()
            step = min(0.04, rem)
            t0 = time.monotonic()
            time.sleep(step)
            rem -= time.monotonic() - t0
        paused_total += self.wait_if_paused()
        return paused_total


class HumanDriver:
    """Simulated human mouse & keyboard interaction driver."""

    def __init__(self, seed: int = 20261123, pause_ctrl: Optional[PauseController] = None) -> None:
        self.rng = random.Random(seed)
        self.prefer_win_space = False
        self.zh_needs_shift = False
        self.zh_strategy: Optional[str] = None
        self.pause_ctrl = pause_ctrl

    def _sleep(self, seconds: float) -> float:
        if self.pause_ctrl is not None:
            return self.pause_ctrl.sleep(seconds)
        time.sleep(seconds)
        return 0.0

    def _wait_if_paused(self) -> float:
        if self.pause_ctrl is not None:
            return self.pause_ctrl.wait_if_paused()
        return 0.0

    def pause(self, low_s: float = 0.18, high_s: float = 0.35) -> None:
        self._sleep(self.rng.uniform(low_s, high_s))

    def move_to(self, tx: int, ty: int, duration: Optional[float] = None, jitter_px: int = 1) -> None:
        self._wait_if_paused()
        sx, sy = get_cursor_pos()
        tx_j = int(tx + self.rng.randint(-jitter_px, jitter_px)) if jitter_px > 0 else int(tx)
        ty_j = int(ty + self.rng.randint(-jitter_px, jitter_px)) if jitter_px > 0 else int(ty)

        def build_curve(
            x0: int, y0: int, dur_hint: Optional[float]
        ) -> tuple[float, tuple[float, float], tuple[float, float], float]:
            dx = float(tx_j - x0)
            dy = float(ty_j - y0)
            dist = math.hypot(dx, dy)
            if dur_hint is None:
                dur = max(0.24, min(1.10, 0.20 + 0.00048 * dist + self.rng.uniform(-0.04, 0.08)))
            else:
                dur = max(0.16, dur_hint)
            if dist < 2.0:
                return dist, (float(x0), float(y0)), (float(tx_j), float(ty_j)), dur
            ux, uy = dx / dist, dy / dist
            nx, ny = -uy, ux
            arc = min(90.0, dist * self.rng.uniform(0.06, 0.16)) * self.rng.choice([-1.0, 1.0])
            cp1 = (
                x0 + dx * self.rng.uniform(0.22, 0.36) + nx * arc * self.rng.uniform(0.6, 1.1),
                y0 + dy * self.rng.uniform(0.22, 0.36) + ny * arc * self.rng.uniform(0.6, 1.1),
            )
            cp2 = (
                x0 + dx * self.rng.uniform(0.65, 0.82) + nx * arc * self.rng.uniform(-0.35, 0.55),
                y0 + dy * self.rng.uniform(0.65, 0.82) + ny * arc * self.rng.uniform(-0.35, 0.55),
            )
            return dist, cp1, cp2, dur

        dist, c1, c2, duration = build_curve(sx, sy, duration)
        if dist < 2.0:
            _mouse_event(0, tx_j, ty_j, move=True)
            return

        step_s = 0.008  # ~125 Hz pointer updates
        t0 = time.perf_counter()
        while True:
            paused_s = self._wait_if_paused()
            if paused_s > 0.0:
                rem = max(0.20, duration - (time.perf_counter() - t0 - paused_s))
                sx, sy = get_cursor_pos()
                dist, c1, c2, duration = build_curve(sx, sy, rem)
                if dist < 2.0:
                    break
                t0 = time.perf_counter()
            elapsed = time.perf_counter() - t0
            if elapsed >= duration:
                break
            u = _min_jerk(elapsed / duration)
            px, py = _cubic_bezier((float(sx), float(sy)), c1, c2, (float(tx_j), float(ty_j)), u)
            _mouse_event(0, int(round(px)), int(round(py)), move=True)
            time.sleep(step_s)
        _mouse_event(0, tx_j, ty_j, move=True)
        self.pause(0.05, 0.12)

    def click_at(
        self, tx: int, ty: int, button: str = "left", duration: Optional[float] = None, double: bool = False
    ) -> None:
        self.move_to(tx, ty, duration=duration)
        self._wait_if_paused()
        if button == "left":
            down, up = MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP
        elif button == "right":
            down, up = MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP
        else:
            down, up = MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP

        clicks = 2 if double else 1
        for k in range(clicks):
            _mouse_event(down)
            time.sleep(self.rng.uniform(0.045, 0.085))
            _mouse_event(up)
            if k + 1 < clicks:
                time.sleep(self.rng.uniform(0.08, 0.13))
        self.pause(0.14, 0.28)

    def drag_middle(self, cx: int, cy: int, dx: int, dy: int, duration: float = 1.4) -> None:
        """Smoothly orbit Creo 3D view using real middle-mouse drag."""
        self.move_to(cx, cy, jitter_px=3)
        self._wait_if_paused()
        _mouse_event(MOUSEEVENTF_MIDDLEDOWN)
        time.sleep(self.rng.uniform(0.08, 0.14))

        sx, sy = get_cursor_pos()
        tx, ty = sx + dx, sy + dy
        dist = max(1.0, math.hypot(dx, dy))
        nx, ny = -dy / dist, dx / dist
        arc = min(32.0, dist * 0.08) * self.rng.choice([-1.0, 1.0])
        c1 = (sx + dx * 0.30 + nx * arc, sy + dy * 0.30 + ny * arc)
        c2 = (sx + dx * 0.72 + nx * arc * 0.5, sy + dy * 0.72 + ny * arc * 0.5)

        t0 = time.perf_counter()
        while True:
            if self.pause_ctrl is not None and self.pause_ctrl.is_pause_requested():
                _mouse_event(MOUSEEVENTF_MIDDLEUP)
                elapsed_before = time.perf_counter() - t0
                self.pause_ctrl.wait_if_paused()
                rem_frac = max(0.0, 1.0 - min(1.0, elapsed_before / max(0.1, duration)))
                if rem_frac > 0.05:
                    self.drag_middle(
                        cx,
                        cy,
                        int(round(dx * rem_frac)),
                        int(round(dy * rem_frac)),
                        duration=max(0.3, duration * rem_frac),
                    )
                return
            elapsed = time.perf_counter() - t0
            if elapsed >= duration:
                break
            u = _min_jerk(elapsed / duration)
            px, py = _cubic_bezier((float(sx), float(sy)), c1, c2, (float(tx), float(ty)), u)
            _mouse_event(0, int(round(px)), int(round(py)), move=True)
            time.sleep(0.01)
        _mouse_event(0, tx, ty, move=True)
        time.sleep(self.rng.uniform(0.09, 0.16))
        _mouse_event(MOUSEEVENTF_MIDDLEUP)
        self.pause(0.22, 0.40)

    def scroll_wheel(self, cx: int, cy: int, total_notches: int, step_delay: float = 0.065) -> None:
        """Zoom smoothly in/out by sending discrete mouse-wheel ticks."""
        self.move_to(cx, cy, jitter_px=4)
        sign = 1 if total_notches >= 0 else -1
        for _ in range(abs(total_notches)):
            self._wait_if_paused()
            _mouse_event(MOUSEEVENTF_WHEEL, data=120 * sign)
            self._sleep(max(0.03, step_delay + self.rng.uniform(-0.015, 0.025)))
        self.pause(0.18, 0.34)

    def _vk_press(self, vk: int, hold_low: float = 0.032, hold_high: float = 0.065) -> None:
        _require_win32()
        scan = user32.MapVirtualKeyW(vk, 0)
        ext = KEYEVENTF_EXTENDEDKEY if vk in (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E) else 0
        down = INPUT(
            type=INPUT_KEYBOARD,
            union=_INPUT_UNION(ki=KEYBDINPUT(wVk=vk, wScan=scan, dwFlags=ext, time=0, dwExtraInfo=0)),
        )
        up = INPUT(
            type=INPUT_KEYBOARD,
            union=_INPUT_UNION(ki=KEYBDINPUT(wVk=vk, wScan=scan, dwFlags=ext | KEYEVENTF_KEYUP, time=0, dwExtraInfo=0)),
        )
        _send_inputs([down])
        time.sleep(self.rng.uniform(hold_low, hold_high))
        _send_inputs([up])

    def press_key(self, vk: int) -> None:
        self._wait_if_paused()
        self._vk_press(vk)
        self._sleep(self.rng.uniform(0.08, 0.15))

    def select_all_and_clear(self) -> None:
        _require_win32()
        ctrl = 0x11
        scan_c = user32.MapVirtualKeyW(ctrl, 0)
        scan_a = user32.MapVirtualKeyW(0x41, 0)
        _send_inputs([
            INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=ctrl, wScan=scan_c, dwFlags=0, time=0, dwExtraInfo=0))),
            INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=0x41, wScan=scan_a, dwFlags=0, time=0, dwExtraInfo=0))),
        ])
        time.sleep(0.05)
        _send_inputs([
            INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=0x41, wScan=scan_a, dwFlags=KEYEVENTF_KEYUP, time=0, dwExtraInfo=0))),
            INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=ctrl, wScan=scan_c, dwFlags=KEYEVENTF_KEYUP, time=0, dwExtraInfo=0))),
        ])
        time.sleep(0.06)
        self._vk_press(0x2E)  # VK_DELETE
        time.sleep(0.09)

    def _focus_hwnd_and_tid(self) -> tuple[int, int, int]:
        _require_win32()
        fg = user32.GetForegroundWindow()
        if not fg:
            return 0, 0, 0
        tid = user32.GetWindowThreadProcessId(fg, None)

        class GUITHREADINFO(ctypes.Structure):
            _fields_ = [
                ("cbSize", wt.DWORD),
                ("flags", wt.DWORD),
                ("hwndActive", wt.HWND),
                ("hwndFocus", wt.HWND),
                ("hwndCapture", wt.HWND),
                ("hwndMenuOwner", wt.HWND),
                ("hwndMoveSize", wt.HWND),
                ("hwndCaret", wt.HWND),
                ("rcCaret", wt.RECT),
            ]

        gti = GUITHREADINFO(cbSize=ctypes.sizeof(GUITHREADINFO))
        focus_hw = fg
        if user32.GetGUIThreadInfo(tid, ctypes.byref(gti)) and gti.hwndFocus:
            focus_hw = int(gti.hwndFocus)
        return int(fg), int(focus_hw), int(tid)

    @staticmethod
    def _installed_hkl(lang_low16: int) -> int:
        _require_win32()
        n = user32.GetKeyboardLayoutList(0, None)
        if n > 0:
            arr = (ctypes.c_void_p * n)()
            user32.GetKeyboardLayoutList(n, arr)
            for val in arr:
                if val and (int(val) & 0xFFFF) == (lang_low16 & 0xFFFF):
                    return int(val)
        return int(lang_low16)

    def _send_chord(self, mod_vk: int, key_vk: int, mod_ext: int = 0, settle_s: float = 0.35) -> None:
        _require_win32()
        sc_m = user32.MapVirtualKeyW(mod_vk, 0)
        sc_k = user32.MapVirtualKeyW(key_vk, 0)
        _send_inputs([INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=mod_vk, wScan=sc_m, dwFlags=mod_ext, time=0, dwExtraInfo=0)))])
        time.sleep(0.05)
        _send_inputs([INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=key_vk, wScan=sc_k, dwFlags=0, time=0, dwExtraInfo=0)))])
        time.sleep(0.05)
        _send_inputs([INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=key_vk, wScan=sc_k, dwFlags=KEYEVENTF_KEYUP, time=0, dwExtraInfo=0)))])
        time.sleep(0.05)
        _send_inputs([INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=mod_vk, wScan=sc_m, dwFlags=mod_ext | KEYEVENTF_KEYUP, time=0, dwExtraInfo=0)))])
        time.sleep(settle_s)

    def _send_win_space(self) -> None:
        self._send_chord(0x5B, 0x20, mod_ext=KEYEVENTF_EXTENDEDKEY, settle_s=0.40)

    def _send_ctrl_space(self) -> None:
        self._send_chord(0xA2, 0x20, mod_ext=0, settle_s=0.30)

    def _send_alt_shift(self) -> None:
        self._send_chord(0xA4, 0xA0, mod_ext=0, settle_s=0.32)

    def send_unicode_text(self, text: str) -> None:
        """Type ASCII/Unicode text safely while preserving MS Pinyin without '.' -> '。' substitution."""
        _require_win32()
        for ch in text:
            self._wait_if_paused()
            if "0" <= ch <= "9":
                self._vk_press(ord(ch), 0.035, 0.065)
            else:
                u_down = INPUT(
                    type=INPUT_KEYBOARD,
                    union=_INPUT_UNION(ki=KEYBDINPUT(wVk=0, wScan=ord(ch), dwFlags=KEYEVENTF_UNICODE, time=0, dwExtraInfo=0)),
                )
                u_up = INPUT(
                    type=INPUT_KEYBOARD,
                    union=_INPUT_UNION(
                        ki=KEYBDINPUT(wVk=0, wScan=ord(ch), dwFlags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, time=0, dwExtraInfo=0)
                    ),
                )
                _send_inputs([u_down])
                time.sleep(self.rng.uniform(0.032, 0.062))
                _send_inputs([u_up])
            if ch in (".", "-", "_"):
                self._sleep(self.rng.uniform(0.14, 0.23))
            else:
                self._sleep(self.rng.uniform(0.08, 0.16))

    def toggle_ime_shift(self) -> None:
        """Tap Left Shift to toggle Microsoft Pinyin Chinese/English sub-mode."""
        self._vk_press(0xA0, 0.04, 0.06)  # VK_LSHIFT
        time.sleep(0.22)

    def _set_imm_open_zh(self, hwnds: Iterable[int]) -> None:
        _require_win32()
        res = ctypes.c_size_t(0)
        for hw in dict.fromkeys(h for h in hwnds if h):
            ime_wnd = imm32.ImmGetDefaultIMEWnd(hw)
            if ime_wnd:
                user32.SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, IMC_SETOPENSTATUS, 1, 2, 300, ctypes.byref(res))
                user32.SendMessageTimeoutW(ime_wnd, WM_IME_CONTROL, IMC_SETCONVERSIONMODE, 0x0401, 2, 300, ctypes.byref(res))

    def ensure_input_language(self, hkl: int, target_hwnd: int = 0) -> None:
        """Switch foreground/focus window between EN (0x0409) and MS Pinyin (0x0804)."""
        _require_win32()
        fg, focus_hw, tid = self._focus_hwnd_and_tid()
        if not fg and not target_hwnd:
            return
        if not tid and target_hwnd:
            tid = int(user32.GetWindowThreadProcessId(target_hwnd, None))
        target_low16 = hkl & 0xFFFF
        real_hkl = self._installed_hkl(target_low16)
        cur_raw = user32.GetKeyboardLayout(tid) if tid else 0
        cur_hkl = int(cur_raw) & 0xFFFFFFFF if cur_raw else 0
        switched_from_other = (cur_hkl & 0xFFFF) != target_low16
        if switched_from_other:
            if not self.prefer_win_space:
                res = ctypes.c_size_t(0)
                for hw in dict.fromkeys(h for h in (target_hwnd, focus_hw, fg) if h):
                    user32.SendMessageTimeoutW(hw, WM_INPUTLANGCHANGEREQUEST, 0, real_hkl, 2, 500, ctypes.byref(res))
                time.sleep(0.16)
                cur_raw = user32.GetKeyboardLayout(tid) if tid else 0
                cur_hkl = int(cur_raw) & 0xFFFFFFFF if cur_raw else 0
            if (cur_hkl & 0xFFFF) != target_low16:
                for _ in range(3):
                    self._send_win_space()
                    cur_raw = user32.GetKeyboardLayout(tid) if tid else 0
                    cur_hkl = int(cur_raw) & 0xFFFFFFFF if cur_raw else 0
                    if (cur_hkl & 0xFFFF) == target_low16:
                        break
        if target_low16 == 0x0804:
            self._set_imm_open_zh((target_hwnd, focus_hw, fg))
            if self.zh_needs_shift and switched_from_other:
                self.toggle_ime_shift()
            time.sleep(0.10)

    def apply_zh_mode(self, strategy: str, target_hwnd: int = 0) -> None:
        """Apply a specific Chinese MS Pinyin activation strategy cleanly."""
        _require_win32()
        fg, focus_hw, tid = self._focus_hwnd_and_tid()
        if not tid and target_hwnd:
            tid = int(user32.GetWindowThreadProcessId(target_hwnd, None))
        real_zh = self._installed_hkl(0x0804)
        res = ctypes.c_size_t(0)
        if strategy == "direct":
            self.ensure_input_language(HKL_ZH_CN, target_hwnd=target_hwnd)
        elif strategy == "shift":
            self.toggle_ime_shift()
        elif strategy == "ctrl_space":
            self._send_ctrl_space()
        elif strategy == "alt_shift":
            for _ in range(3):
                self._send_alt_shift()
                cur = int(user32.GetKeyboardLayout(tid) or 0) & 0xFFFF if tid else 0
                if cur == 0x0804:
                    break
            self._set_imm_open_zh((target_hwnd, focus_hw, fg))
        elif strategy == "win_space":
            for _ in range(4):
                self._send_win_space()
                cur = int(user32.GetKeyboardLayout(tid) or 0) & 0xFFFF if tid else 0
                if cur == 0x0804:
                    break
            time.sleep(0.12)
        elif strategy == "win_space_shift":
            self.toggle_ime_shift()
        elif strategy == "msg_0050":
            for hw in dict.fromkeys(h for h in (target_hwnd, focus_hw, fg) if h):
                user32.SendMessageTimeoutW(hw, WM_INPUTLANGCHANGEREQUEST, 0, real_zh, 2, 500, ctypes.byref(res))
            time.sleep(0.16)
            self._set_imm_open_zh((target_hwnd, focus_hw, fg))
        elif strategy == "msg_0050_shift":
            self.toggle_ime_shift()

    def calibrate_ime_on_temp_window(self, probe_pinyin_word: str = "dianji") -> tuple[bool, str, str]:
        """
        Open a tiny temporary Win32 EDIT window for ~1.5s to verify real MS Pinyin
        Chinese composition via SendInput before launching Creo/FFmpeg.
        Returns (ok, observed_text, mode_summary).
        """
        _require_win32()
        kernel32 = ctypes.windll.kernel32
        hinst = kernel32.GetModuleHandleW(None)
        ed = user32.CreateWindowExW(
            0x00000008,  # WS_EX_TOPMOST
            "EDIT",
            "IME Probe",
            0x10CF0080,  # WS_OVERLAPPEDWINDOW|WS_VISIBLE|ES_AUTOHSCROLL
            60,
            60,
            340,
            90,
            None,
            None,
            hinst,
            None,
        )
        if not ed:
            return False, "", "CreateWindowExW failed"

        result = {"ok": False, "text": "", "mode": "untested", "done": False}

        def read_edit() -> str:
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(ed, buf, 256)
            return buf.value

        def clear_edit() -> None:
            user32.SetWindowTextW(ed, "")
            time.sleep(0.08)

        def finish(ok: bool, text: str, mode: str) -> None:
            result.update({"ok": ok, "text": text, "mode": mode, "done": True})

        def probe_pinyin() -> str:
            for ch in probe_pinyin_word:
                self._vk_press(ord(ch.upper()), 0.03, 0.05)
                time.sleep(0.05)
            time.sleep(0.20)
            self._vk_press(0x20, 0.04, 0.06)
            time.sleep(0.20)
            return read_edit()

        def worker() -> None:
            time.sleep(0.25)
            user32.SetForegroundWindow(ed)
            user32.SetFocus(ed)
            time.sleep(0.15)
            self.type_ascii("1.4", switch_en=True)
            clear_edit()
            for mode_name in ("direct", "shift", "ctrl_space", "alt_shift", "win_space", "win_space_shift"):
                clear_edit()
                self.apply_zh_mode(mode_name, target_hwnd=ed)
                txt = probe_pinyin()
                if any("\u4e00" <= c <= "\u9fff" for c in txt):
                    self.prefer_win_space = "win_space" in mode_name
                    self.zh_needs_shift = "shift" in mode_name and mode_name != "alt_shift"
                    self.zh_strategy = mode_name
                    clear_edit()
                    self.type_ascii("1.4", switch_en=False)
                    num_obs = read_edit()
                    finish(True, f"{txt}|{num_obs}", mode_name)
                    return
            self.prefer_win_space = False
            self.zh_needs_shift = False
            finish(False, read_edit(), "failed")

        t = threading.Thread(target=worker, daemon=True)
        t.start()
        msg = wt.MSG()
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline and not result["done"] and user32.IsWindow(ed):
            while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                if msg.message == 0x0012:  # WM_QUIT
                    break
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.01)
        if user32.IsWindow(ed):
            user32.DestroyWindow(ed)
        return bool(result["ok"]), str(result["text"]), str(result["mode"])

    def type_ascii(self, text: str, switch_en: bool = False) -> None:
        """
        Type ASCII digits/letters/operators with human cadence.
        When `switch_en` is False (default), keeps active MS Pinyin (0x0804) intact.
        """
        if not switch_en:
            self.send_unicode_text(text)
            return
        _require_win32()
        self.ensure_input_language(HKL_EN_US)
        for ch in text:
            res = user32.VkKeyScanW(ord(ch))
            if res == -1:
                self.send_unicode_text(ch)
                continue
            vk = res & 0xFF
            shift = (res >> 8) & 1
            if shift:
                _send_inputs(
                    [INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=0x10, wScan=user32.MapVirtualKeyW(0x10, 0), dwFlags=0, time=0, dwExtraInfo=0)))]
                )
                time.sleep(0.025)
            self._vk_press(vk, 0.035, 0.070)
            if shift:
                time.sleep(0.025)
                _send_inputs(
                    [INPUT(type=INPUT_KEYBOARD, union=_INPUT_UNION(ki=KEYBDINPUT(wVk=0x10, wScan=user32.MapVirtualKeyW(0x10, 0), dwFlags=KEYEVENTF_KEYUP, time=0, dwExtraInfo=0)))]
                )
            if ch in (".", "-", "_"):
                time.sleep(self.rng.uniform(0.14, 0.23))
            else:
                time.sleep(self.rng.uniform(0.08, 0.16))

    def type_zh_phrase(self, token: str, switch_zh: bool = True, target_hwnd: int = 0) -> None:
        """Type a single Pinyin phrase and press Space to commit MS Pinyin candidate #1."""
        self._wait_if_paused()
        ctx = self.pause_ctrl.critical_section() if self.pause_ctrl is not None else contextlib.nullcontext()
        with ctx:
            if switch_zh:
                if self.zh_strategy and self.zh_strategy not in ("direct", "unicode"):
                    _fg, _focus_hw, tid = self._focus_hwnd_and_tid()
                    if not tid and target_hwnd:
                        tid = int(user32.GetWindowThreadProcessId(target_hwnd, None))
                    cur = int(user32.GetKeyboardLayout(tid) or 0) & 0xFFFF if tid else 0
                    if cur != 0x0804:
                        self.apply_zh_mode(self.zh_strategy, target_hwnd=target_hwnd)
                else:
                    self.ensure_input_language(HKL_ZH_CN, target_hwnd=target_hwnd)
            for letter in token:
                vk = ord(letter.upper())
                self._vk_press(vk, 0.035, 0.075)
                time.sleep(self.rng.uniform(0.065, 0.135))
            time.sleep(self.rng.uniform(0.22, 0.38))
            self._vk_press(0x20, 0.040, 0.075)  # VK_SPACE selects 1st candidate
            time.sleep(self.rng.uniform(0.20, 0.42))
        self._wait_if_paused()

    def type_ime_phrases(self, segments: Iterable[tuple[str, str]]) -> None:
        """Type Chinese/ASCII mixed text with word-segmented human Pinyin cadence."""
        for kind, token in segments:
            if kind == "zh":
                self.type_zh_phrase(token, switch_zh=True)
            else:
                self.type_ascii(token, switch_en=True)
                time.sleep(self.rng.uniform(0.16, 0.30))


class UIADriver:
    """Thin IUIAutomation wrapper for finding Creo window, Ribbon buttons, tree rows, and dialog widgets."""

    def __init__(self) -> None:
        client, uia_mod = _load_uia_module()
        self.uia_mod = uia_mod
        self.uia = client.CreateObject(uia_mod.CUIAutomation._reg_clsid_, interface=uia_mod.IUIAutomation)

    def find_creo_window(self, title_substr: str = "Creo Parametric", timeout_s: float = 45.0) -> tuple[int, object]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            root = self.uia.GetRootElement()
            cond = self.uia.CreatePropertyCondition(
                self.uia_mod.UIA_ControlTypePropertyId, self.uia_mod.UIA_WindowControlTypeId
            )
            wins = root.FindAll(self.uia_mod.TreeScope_Children, cond)
            for i in range(wins.Length):
                w = wins.GetElement(i)
                name = w.CurrentName or ""
                if title_substr in name:
                    hwnd = int(w.CurrentNativeWindowHandle)
                    if hwnd:
                        return hwnd, w
            time.sleep(0.5)
        raise TimeoutError(f"Creo window matching '{title_substr}' not found within {timeout_s}s")

    def maximize_and_focus(self, hwnd: int) -> tuple[int, int, int, int]:
        _require_win32()
        user32.ShowWindow(hwnd, 3)  # SW_MAXIMIZE
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        return int(rect.left), int(rect.top), int(rect.right), int(rect.bottom)

    def find_descendants(
        self,
        parent: object,
        name_contains: Optional[str] = None,
        control_type: Optional[int] = None,
        automation_id: Optional[str] = None,
    ) -> list[object]:
        if control_type is not None:
            cond = self.uia.CreatePropertyCondition(self.uia_mod.UIA_ControlTypePropertyId, control_type)
        else:
            cond = self.uia.CreateTrueCondition()
        arr = parent.FindAll(self.uia_mod.TreeScope_Descendants, cond)
        out: list[object] = []
        for i in range(arr.Length):
            el = arr.GetElement(i)
            try:
                nm = el.CurrentName or ""
                aid = el.CurrentAutomationId or ""
                if name_contains and name_contains not in nm:
                    continue
                if automation_id and automation_id != aid:
                    continue
                r = el.CurrentBoundingRectangle
                if (r.right - r.left) <= 0 or (r.bottom - r.top) <= 0:
                    continue
                out.append(el)
            except Exception:
                continue
        return out

    @staticmethod
    def center_of(el: object) -> tuple[int, int]:
        r = el.CurrentBoundingRectangle
        return int((r.left + r.right) // 2), int((r.top + r.bottom) // 2)


def find_ffmpeg_exe() -> str:
    on_path = shutil.which("ffmpeg")
    if on_path:
        return on_path
    winget_root = pathlib.Path.home() / "AppData" / "Local" / "Microsoft" / "WinGet"
    if winget_root.is_dir():
        for p in winget_root.rglob("ffmpeg.exe"):
            if p.is_file():
                return str(p)
    raise FileNotFoundError("ffmpeg.exe not found on PATH or under LocalAppData WinGet")


class FFmpegRecorder:
    """Window/desktop region recorder backed by FFmpeg `gdigrab` + H.264 MP4 with lossless pause/resume segments."""

    def __init__(
        self,
        out_mp4: pathlib.Path,
        rect: tuple[int, int, int, int],
        fps: int = 30,
        target_size: tuple[int, int] = (1920, 1080),
        segments_dir: Optional[pathlib.Path] = None,
        ffmpeg_exe: Optional[str] = None,
        virtual_bounds: Optional[tuple[int, int, int, int]] = None,
    ) -> None:
        self.ffmpeg = ffmpeg_exe if ffmpeg_exe is not None else find_ffmpeg_exe()
        self.out_mp4 = pathlib.Path(out_mp4).resolve()
        self.out_mp4.parent.mkdir(parents=True, exist_ok=True)
        if virtual_bounds is None:
            _require_win32()
            virtual_bounds = (
                int(user32.GetSystemMetrics(76)),
                int(user32.GetSystemMetrics(77)),
                int(user32.GetSystemMetrics(78)),
                int(user32.GetSystemMetrics(79)),
            )
        self.crop = clamp_capture_rect(rect, virtual_bounds)
        self.fps = fps
        self.target_size = target_size
        self._segments_dir = (
            pathlib.Path(segments_dir).resolve()
            if segments_dir is not None
            else (self.out_mp4.parent / f"._{self.out_mp4.stem}_parts")
        )
        self._segments: list[pathlib.Path] = []
        self._seg_idx = 0
        self._io_lock = threading.Lock()
        self._log_fh = None
        self.proc: Optional[subprocess.Popen] = None

    def build_ffmpeg_command(self, seg_path: pathlib.Path) -> list[str]:
        x, y, w, h = self.crop
        tw, th = self.target_size
        vf = (
            f"scale={tw}:{th}:force_original_aspect_ratio=decrease:flags=lanczos,"
            f"pad={tw}:{th}:(ow-iw)/2:(oh-ih)/2,setsar=1"
        )
        return [
            self.ffmpeg,
            "-y",
            "-f",
            "gdigrab",
            "-framerate",
            str(self.fps),
            "-draw_mouse",
            "1",
            "-offset_x",
            str(x),
            "-offset_y",
            str(y),
            "-video_size",
            f"{w}x{h}",
            "-i",
            "desktop",
            "-vf",
            vf,
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "18",
            "-pix_fmt",
            "yuv420p",
            str(seg_path),
        ]

    def _spawn_segment(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            return
        self._segments_dir.mkdir(parents=True, exist_ok=True)
        seg_path = self._segments_dir / f"seg_{self._seg_idx:03d}.mp4"
        log_path = self._segments_dir / f"seg_{self._seg_idx:03d}.ffmpeg.log"
        self._seg_idx += 1

        cmd = self.build_ffmpeg_command(seg_path)
        self._log_fh = open(log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=self._log_fh,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0,
        )
        time.sleep(0.7)
        if self.proc.poll() is not None:
            rc = self.proc.returncode
            self._finish_current_segment()
            raise RuntimeError(f"FFmpeg exited prematurely with code {rc}; see {log_path}")
        self._segments.append(seg_path)

    def _finish_current_segment(self) -> None:
        proc = self.proc
        self.proc = None
        if proc is not None and proc.poll() is None:
            try:
                if proc.stdin:
                    proc.stdin.write(b"q\n")
                    proc.stdin.flush()
                proc.wait(timeout=15)
            except Exception:
                proc.terminate()
                proc.wait(timeout=5)
        if self._log_fh is not None:
            try:
                self._log_fh.close()
            except Exception:
                pass
            self._log_fh = None

    def start(self) -> None:
        with self._io_lock:
            shutil.rmtree(self._segments_dir, ignore_errors=True)
            self._segments.clear()
            self._seg_idx = 0
            self._spawn_segment()

    def pause(self) -> None:
        with self._io_lock:
            self._finish_current_segment()

    def resume(self) -> None:
        with self._io_lock:
            self._spawn_segment()

    def stop(self, finalize: bool = True) -> None:
        with self._io_lock:
            self._finish_current_segment()
            valid = [p for p in self._segments if p.is_file() and p.stat().st_size > 0]
            if finalize and valid:
                if self.out_mp4.exists():
                    self.out_mp4.unlink()
                if len(valid) == 1:
                    shutil.move(str(valid[0]), str(self.out_mp4))
                else:
                    concat_txt = self._segments_dir / "concat.txt"
                    concat_txt.write_text(
                        "\n".join(f"file '{p.name}'" for p in valid) + "\n",
                        encoding="utf-8",
                    )
                    res = subprocess.run(
                        [
                            self.ffmpeg,
                            "-y",
                            "-f",
                            "concat",
                            "-safe",
                            "0",
                            "-i",
                            "concat.txt",
                            "-c",
                            "copy",
                            "-movflags",
                            "+faststart",
                            str(self.out_mp4),
                        ],
                        cwd=str(self._segments_dir),
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        errors="replace",
                    )
                    if res.returncode != 0 or not self.out_mp4.is_file():
                        raise RuntimeError(
                            f"FFmpeg concat failed (code={res.returncode}): {res.stderr[-600:]}"
                        )
            shutil.rmtree(self._segments_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Human-like Windows UI & FFmpeg recording utilities")
    parser.add_argument("--test-ime", action="store_true", help="Run temporary Win32 EDIT MS Pinyin probe")
    parser.add_argument("--check-ffmpeg", action="store_true", help="Verify ffmpeg.exe discovery")
    args = parser.parse_args()

    if args.check_ffmpeg:
        print(f"FFMPEG_EXE={find_ffmpeg_exe()}")
    if args.test_ime:
        drv = HumanDriver()
        ok, text, mode = drv.calibrate_ime_on_temp_window()
        print(f"IME_PROBE ok={ok} mode={mode} text={text!r}")
        return 0 if ok else 1
    if not args.check_ffmpeg and not args.test_ime:
        parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
