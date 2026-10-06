"""The monitor a window opens on, for its first size and place.

Tk knows one screen: on X11 that is every monitor together (two side by
side make one wide screen), so "90% of the screen" could be wider than the
monitor the window lands on. Here: the work area of the monitor under the
mouse, where window managers and Windows open a new window -- on Windows
less the taskbar (GetMonitorInfo), on X11 the monitor XRandR reports.
Anything that fails gives Tk's whole screen, as before."""
from __future__ import annotations

import ctypes
import ctypes.util
import sys


def pick(monitors, pointer):
    """The (x, y, width, height, primary) monitor holding the pointer, else
    the primary one, else the first; None when there are none."""
    px, py = pointer
    for x, y, w, h, _ in monitors:
        if x <= px < x + w and y <= py < y + h:
            return x, y, w, h
    for x, y, w, h, primary in monitors:
        if primary:
            return x, y, w, h
    return tuple(monitors[0][:4]) if monitors else None


def work_area(window):
    """(x, y, width, height) in the window's screen coordinates."""
    try:
        if sys.platform == "win32":
            found = _windows_work_area()
        elif window.tk.call("tk", "windowingsystem") == "x11":
            found = pick(_x11_monitors(), window.winfo_pointerxy())
        else:
            found = None
    except Exception:       # ctypes, a missing library, an old server: Tk's screen instead
        found = None
    if found and found[2] > 0 and found[3] > 0:
        return found
    return 0, 0, window.winfo_screenwidth(), window.winfo_screenheight()


def fit(window):
    """Size a dialog as it asks, but no more than 90% of the monitor's work
    area, in its middle: a tall one would otherwise run off the bottom of a
    1080p screen, buttons and all."""
    window.update_idletasks()
    x, y, width, height = work_area(window)
    w = min(window.winfo_reqwidth(), width * 9 // 10)
    h = min(window.winfo_reqheight(), height * 9 // 10)
    window.geometry(f"{w}x{h}+{x + (width - w) // 2}+{y + (height - h) // 2}")


class _Rect(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_ulong), ("rcMonitor", _Rect), ("rcWork", _Rect),
                ("dwFlags", ctypes.c_ulong)]


class _Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def _windows_work_area():
    """The work area of the monitor under the cursor (the primary one when
    the cursor is nowhere), in the dpi-aware pixels Tk uses (theme.dpi_awareness)."""
    user32 = ctypes.windll.user32
    user32.MonitorFromPoint.restype = ctypes.c_void_p
    user32.MonitorFromPoint.argtypes = [_Point, ctypes.c_ulong]
    user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.POINTER(_MonitorInfo)]
    point = _Point()
    if not user32.GetCursorPos(ctypes.byref(point)):
        point = _Point(0, 0)
    monitor = user32.MonitorFromPoint(point, 1)        # MONITOR_DEFAULTTOPRIMARY
    info = _MonitorInfo()
    info.cbSize = ctypes.sizeof(_MonitorInfo)
    if not monitor or not user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
        return None
    r = info.rcWork
    return r.left, r.top, r.right - r.left, r.bottom - r.top


class _XRRMonitorInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_ulong), ("primary", ctypes.c_int), ("automatic", ctypes.c_int),
                ("noutput", ctypes.c_int), ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("mwidth", ctypes.c_int), ("mheight", ctypes.c_int),
                ("outputs", ctypes.c_void_p)]


def _load(name):
    path = ctypes.util.find_library(name)
    for candidate in ([path] if path else []) + [f"lib{name}.so.2", f"lib{name}.so.6", f"lib{name}.so"]:
        try:
            return ctypes.CDLL(candidate)
        except OSError:
            continue
    raise OSError(f"no lib{name}")


def _x11_monitors():
    """[(x, y, width, height, primary)] of the active monitors (XRandR 1.5)."""
    x11, xrandr = _load("X11"), _load("Xrandr")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xrandr.XRRGetMonitors.restype = ctypes.POINTER(_XRRMonitorInfo)
    xrandr.XRRGetMonitors.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_int)]
    xrandr.XRRFreeMonitors.argtypes = [ctypes.POINTER(_XRRMonitorInfo)]
    display = x11.XOpenDisplay(None)
    if not display:
        return []
    try:
        count = ctypes.c_int(0)
        found = xrandr.XRRGetMonitors(display, x11.XDefaultRootWindow(display), 1, ctypes.byref(count))
        if not found:
            return []
        try:
            return [(m.x, m.y, m.width, m.height, bool(m.primary)) for m in found[:count.value]]
        finally:
            xrandr.XRRFreeMonitors(found)
    finally:
        x11.XCloseDisplay(display)
