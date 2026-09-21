"""Pitch stills: same GoW frame, mini overlay + plate, real 1440 session.

DPI-aware so we don't grab 1280x720 of the IDE. Largest session HWND only.
NR-on modes wait for NeuralUplift + a ReShade log line before shooting.
"""
from __future__ import annotations

import ctypes
import shutil
import subprocess
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image, ImageGrab, ImageStat

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

user32 = ctypes.windll.user32


class RECT(ctypes.Structure):
    _fields_ = (("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long),
                ("bottom", ctypes.c_long))


EXE = Path(r"C:\Users\Leo\Documents\NexusCap\build\Release\NexusCap.exe")
WORK = EXE.parent
PITCH = Path(r"C:\Users\Leo\Documents\NexusCap\assets\pitch")
SETTINGS = Path(r"C:\Users\Leo\AppData\Local\NexusCap\settings.ini")
BAK = SETTINGS.with_suffix(".ini.pitchbak")
LOG = WORK / "ReShade.log"

MODES = [
    ("nroff", "print_compare_nroff.jpg", 0, 1, ("print_compare_nr_off.jpg",)),
    ("1x", "print_compare_1x.jpg", 1, 1, ("print_compare_nr_on.jpg",)),
    ("15x", "print_compare_15x.jpg", 1, 4, ("print_session_nr_on.jpg",)),
    ("2x", "print_compare_2x.jpg", 1, 2, ()),
    ("2xopt", "print_compare_2xopt.jpg", 1, 5, ()),
    ("3x", "print_compare_3x.jpg", 1, 3, ()),
]


def set_ini(**keys: object) -> None:
    lines = SETTINGS.read_text(encoding="utf-8").splitlines()
    section = ""
    out = []
    for ln in lines:
        if ln.startswith("[") and ln.endswith("]"):
            section = ln[1:-1]
        wrote = False
        if section == "session":
            for k, v in keys.items():
                if ln.startswith(f"{k}="):
                    out.append(f"{k}={v}")
                    wrote = True
                    break
        if not wrote:
            out.append(ln)
    SETTINGS.write_text("\n".join(out) + "\n", encoding="utf-8")


def stop_nc() -> None:
    subprocess.run(["taskkill", "/IM", "NexusCap.exe", "/F"], capture_output=True)
    time.sleep(2)


def session_hwnd() -> int:
    buf = ctypes.create_unicode_buffer(512)
    best, best_area = 0, 0

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lp):
        nonlocal best, best_area
        if not user32.IsWindowVisible(hwnd):
            return True
        user32.GetWindowTextW(hwnd, buf, 512)
        title = buf.value or ""
        if "Nexus Cap v" not in title or "Console" in title:
            return True
        rc = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rc))
        w, h = rc.right - rc.left, rc.bottom - rc.top
        area = w * h
        if w >= 1920 and h >= 1000 and area > best_area:
            best, best_area = int(hwnd), area
        return True

    user32.EnumWindows(cb, 0)
    return best


def window_bbox(hwnd: int) -> tuple[int, int, int, int]:
    rc = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    return rc.left, rc.top, rc.right, rc.bottom


def wait_session(timeout: float = 80.0) -> int:
    t0 = time.time()
    while time.time() - t0 < timeout:
        hwnd = session_hwnd()
        if hwnd:
            return hwnd
        time.sleep(0.5)
    return 0


def log_has_nr(since: float) -> bool:
    if not LOG.is_file():
        return False
    try:
        text = LOG.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False
    tail = text[-80000:]
    keys = ("EvaluateFeature", "NR PASS", "NR toggled ON", "NeuralUplift", "toggled ON")
    return any(k in tail for k in keys)


def wait_nr(want_on: bool, timeout: float = 25.0) -> None:
    if not want_on:
        time.sleep(8)
        return
    t0 = time.time()
    while time.time() - t0 < timeout:
        if log_has_nr(t0):
            time.sleep(3)
            return
        time.sleep(0.5)
    time.sleep(6)


def grab(path: Path, hwnd: int) -> None:
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.6)
    box = window_bbox(hwnd)
    w, h = box[2] - box[0], box[3] - box[1]
    if w < 1920 or h < 1000:
        raise RuntimeError(f"session too small {w}x{h} box={box}")
    img = ImageGrab.grab(bbox=box, all_screens=True)
    if img.size[0] < 1920 or img.size[1] < 1000:
        raise RuntimeError(f"grab too small {img.size} box={box}")
    stat = ImageStat.Stat(img.convert("L"))
    if stat.mean[0] < 8:
        raise RuntimeError(f"grab too dark mean={stat.mean[0]:.1f}")
    img.convert("RGB").save(path, "JPEG", quality=92, optimize=True)
    print(f"wrote {path.name} {path.stat().st_size} {img.size} box={box} mean={stat.mean[0]:.1f}",
          flush=True)


def main() -> None:
    shutil.copy2(SETTINGS, BAK)
    try:
        for name, filename, nr, pass_n, aliases in MODES:
            print(f"==== {name} pass={pass_n} nr={nr} ====", flush=True)
            stop_nc()
            set_ini(
                enhance_renodx=1,
                neural_rendering=nr,
                nr_pass_count=pass_n,
                fg_enabled=0,
                show_fps=1,
                overlay_no_bg=0,
                mini_overlay=1,
                advanced_metrics=0,
                hw_info=1,
                live_overlay=0,
                fullscreen=1,
                scale_percent=100,
                preview_cfg_live=0,
                hide_panel_on_capture=1,
            )
            subprocess.Popen([str(EXE), "--autostart"], cwd=str(WORK))
            hwnd = wait_session()
            if not hwnd:
                raise SystemExit(f"session 1440 missing for {name}")
            wait_nr(want_on=bool(nr))
            live = session_hwnd()
            if live:
                hwnd = live
            dest = PITCH / filename
            last_err = None
            for attempt in range(4):
                try:
                    grab(dest, hwnd)
                    last_err = None
                    break
                except RuntimeError as exc:
                    last_err = exc
                    print(f"retry {name} {attempt+1}: {exc}", flush=True)
                    time.sleep(2)
                    live = session_hwnd()
                    if live:
                        hwnd = live
                    elif not hwnd:
                        hwnd = wait_session(30)
            if last_err:
                raise last_err
            for alias in aliases:
                shutil.copy2(dest, PITCH / alias)
    finally:
        stop_nc()
        if BAK.is_file():
            shutil.copy2(BAK, SETTINGS)
            BAK.unlink()
        print("settings restored", flush=True)


if __name__ == "__main__":
    main()
