from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes
from typing import Iterable

from .types import HWND, Rect


SW_HIDE = 0
SW_NORMAL = 1
SW_MAXIMIZE = 3
SW_SHOW = 5
SW_MINIMIZE = 6
SW_RESTORE = 9

SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004

WM_CLOSE = 0x0010

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
INPUT_HARDWARE = 2

SM_CXSCREEN = 0
SM_CYSCREEN = 1
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

DWMWA_EXTENDED_FRAME_BOUNDS = 9
BI_RGB = 0
DIB_RGB_COLORS = 0
SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000


ULONG_PTR = ctypes.c_size_t


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
        ("hi", HARDWAREINPUT),
    ]


class INPUT(ctypes.Structure):
    _anonymous_ = ("value",)
    _fields_ = [
        ("type", wintypes.DWORD),
        ("value", _INPUTUNION),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class RGBQUAD(ctypes.Structure):
    _fields_ = [
        ("rgbBlue", wintypes.BYTE),
        ("rgbGreen", wintypes.BYTE),
        ("rgbRed", wintypes.BYTE),
        ("rgbReserved", wintypes.BYTE),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [
        ("bmiHeader", BITMAPINFOHEADER),
        ("bmiColors", RGBQUAD * 1),
    ]


_IS_WINDOWS = sys.platform == "win32"
_user32: ctypes.WinDLL | None = None  # type: ignore[attr-defined]
_gdi32: ctypes.WinDLL | None = None  # type: ignore[attr-defined]
_dwmapi: ctypes.WinDLL | None = None  # type: ignore[attr-defined]


if _IS_WINDOWS:
    _user32 = ctypes.WinDLL("user32", use_last_error=True)
    _gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
    _dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    _user32.IsWindow.argtypes = (wintypes.HWND,)
    _user32.IsWindow.restype = wintypes.BOOL
    _user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    _user32.IsWindowVisible.restype = wintypes.BOOL
    _user32.IsWindowEnabled.argtypes = (wintypes.HWND,)
    _user32.IsWindowEnabled.restype = wintypes.BOOL
    _user32.IsIconic.argtypes = (wintypes.HWND,)
    _user32.IsIconic.restype = wintypes.BOOL

    _user32.EnumWindows.argtypes = (WNDENUMPROC, wintypes.LPARAM)
    _user32.EnumWindows.restype = wintypes.BOOL
    _user32.EnumChildWindows.argtypes = (wintypes.HWND, WNDENUMPROC, wintypes.LPARAM)
    _user32.EnumChildWindows.restype = wintypes.BOOL

    _user32.GetWindowTextLengthW.argtypes = (wintypes.HWND,)
    _user32.GetWindowTextLengthW.restype = ctypes.c_int
    _user32.GetWindowTextW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    _user32.GetWindowTextW.restype = ctypes.c_int
    _user32.GetClassNameW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
    _user32.GetClassNameW.restype = ctypes.c_int

    _user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
    _user32.GetWindowRect.restype = wintypes.BOOL
    _user32.GetParent.argtypes = (wintypes.HWND,)
    _user32.GetParent.restype = wintypes.HWND
    _user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    _user32.GetWindowThreadProcessId.restype = wintypes.DWORD

    _user32.GetForegroundWindow.argtypes = ()
    _user32.GetForegroundWindow.restype = wintypes.HWND
    _user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
    _user32.SetForegroundWindow.restype = wintypes.BOOL
    _user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    _user32.ShowWindow.restype = wintypes.BOOL
    _user32.SetWindowPos.argtypes = (
        wintypes.HWND,
        wintypes.HWND,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.UINT,
    )
    _user32.SetWindowPos.restype = wintypes.BOOL
    _user32.PostMessageW.argtypes = (
        wintypes.HWND,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
    )
    _user32.PostMessageW.restype = wintypes.BOOL

    _user32.GetSystemMetrics.argtypes = (ctypes.c_int,)
    _user32.GetSystemMetrics.restype = ctypes.c_int
    _user32.GetDoubleClickTime.argtypes = ()
    _user32.GetDoubleClickTime.restype = wintypes.UINT
    _user32.SendInput.argtypes = (
        wintypes.UINT,
        ctypes.POINTER(INPUT),
        ctypes.c_int,
    )
    _user32.SendInput.restype = wintypes.UINT

    _user32.GetDC.argtypes = (wintypes.HWND,)
    _user32.GetDC.restype = wintypes.HDC
    _user32.ReleaseDC.argtypes = (wintypes.HWND, wintypes.HDC)
    _user32.ReleaseDC.restype = ctypes.c_int

    _user32.SetProcessDpiAwarenessContext.argtypes = (ctypes.c_void_p,)
    _user32.SetProcessDpiAwarenessContext.restype = wintypes.BOOL

    _gdi32.CreateCompatibleDC.argtypes = (wintypes.HDC,)
    _gdi32.CreateCompatibleDC.restype = wintypes.HDC
    _gdi32.DeleteDC.argtypes = (wintypes.HDC,)
    _gdi32.DeleteDC.restype = wintypes.BOOL
    _gdi32.CreateDIBSection.argtypes = (
        wintypes.HDC,
        ctypes.POINTER(BITMAPINFO),
        wintypes.UINT,
        ctypes.POINTER(ctypes.c_void_p),
        wintypes.HANDLE,
        wintypes.DWORD,
    )
    _gdi32.CreateDIBSection.restype = wintypes.HBITMAP
    _gdi32.SelectObject.argtypes = (wintypes.HDC, wintypes.HGDIOBJ)
    _gdi32.SelectObject.restype = wintypes.HGDIOBJ
    _gdi32.DeleteObject.argtypes = (wintypes.HGDIOBJ,)
    _gdi32.DeleteObject.restype = wintypes.BOOL
    _gdi32.BitBlt.argtypes = (
        wintypes.HDC,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.HDC,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.DWORD,
    )
    _gdi32.BitBlt.restype = wintypes.BOOL

    _dwmapi.DwmGetWindowAttribute.argtypes = (
        wintypes.HWND,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.DWORD,
    )
    _dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long


def _require_windows() -> None:
    if not _IS_WINDOWS:
        raise OSError("litewinwrap Win32 operations require Windows")


def _last_error(api: str) -> OSError:
    error = ctypes.get_last_error()
    if error:
        return ctypes.WinError(error)
    return OSError(f"{api} failed without setting a Win32 error")


def enable_per_monitor_dpi_awareness() -> bool:
    """Request per-monitor-v2 DPI awareness before creating or querying windows."""

    _require_windows()
    assert _user32 is not None
    context = ctypes.c_void_p(-4)  # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    ctypes.set_last_error(0)
    if _user32.SetProcessDpiAwarenessContext(context):
        return True
    if ctypes.get_last_error() == 5:  # Already fixed by a manifest or earlier API call.
        return False
    raise _last_error("SetProcessDpiAwarenessContext")


def is_window(hwnd: HWND | int) -> bool:
    _require_windows()
    assert _user32 is not None
    return bool(_user32.IsWindow(int(hwnd)))


def is_window_visible(hwnd: HWND | int) -> bool:
    _require_windows()
    assert _user32 is not None
    return bool(_user32.IsWindowVisible(int(hwnd)))


def is_window_enabled(hwnd: HWND | int) -> bool:
    _require_windows()
    assert _user32 is not None
    return bool(_user32.IsWindowEnabled(int(hwnd)))


def is_iconic(hwnd: HWND | int) -> bool:
    _require_windows()
    assert _user32 is not None
    return bool(_user32.IsIconic(int(hwnd)))


def _enum_windows(parent: HWND | int | None) -> tuple[HWND, ...]:
    _require_windows()
    assert _user32 is not None
    handles: list[HWND] = []

    @WNDENUMPROC
    def callback(hwnd: int, _parameter: int) -> bool:
        handles.append(HWND(hwnd))
        return True

    ctypes.set_last_error(0)
    if parent is None:
        ok = _user32.EnumWindows(callback, 0)
    else:
        ok = _user32.EnumChildWindows(int(parent), callback, 0)
    if not ok:
        raise _last_error("EnumWindows" if parent is None else "EnumChildWindows")
    return tuple(handles)


def enum_windows() -> tuple[HWND, ...]:
    return _enum_windows(None)


def enum_child_windows(hwnd: HWND | int) -> tuple[HWND, ...]:
    return _enum_windows(hwnd)


def get_window_text(hwnd: HWND | int) -> str:
    _require_windows()
    assert _user32 is not None
    length = _user32.GetWindowTextLengthW(int(hwnd))
    buffer = ctypes.create_unicode_buffer(max(1, length + 1))
    copied = _user32.GetWindowTextW(int(hwnd), buffer, len(buffer))
    return buffer.value[:copied]


def get_class_name(hwnd: HWND | int) -> str:
    _require_windows()
    assert _user32 is not None
    capacity = 256
    buffer = ctypes.create_unicode_buffer(capacity)
    copied = _user32.GetClassNameW(int(hwnd), buffer, capacity)
    if not copied and not is_window(hwnd):
        raise LookupError(f"Window no longer exists: {int(hwnd)}")
    return buffer.value[:copied]


def get_window_rect(hwnd: HWND | int) -> Rect:
    _require_windows()
    assert _user32 is not None
    result = wintypes.RECT()
    ctypes.set_last_error(0)
    if not _user32.GetWindowRect(int(hwnd), ctypes.byref(result)):
        raise _last_error("GetWindowRect")
    return Rect(result.left, result.top, result.right, result.bottom)


def get_extended_frame_rect(hwnd: HWND | int) -> Rect:
    _require_windows()
    assert _dwmapi is not None
    result = wintypes.RECT()
    status = _dwmapi.DwmGetWindowAttribute(
        int(hwnd),
        DWMWA_EXTENDED_FRAME_BOUNDS,
        ctypes.byref(result),
        ctypes.sizeof(result),
    )
    if status == 0:
        return Rect(result.left, result.top, result.right, result.bottom)
    return get_window_rect(hwnd)


def get_parent(hwnd: HWND | int) -> HWND | None:
    _require_windows()
    assert _user32 is not None
    parent = _user32.GetParent(int(hwnd))
    return HWND(parent) if parent else None


def get_window_process_id(hwnd: HWND | int) -> int:
    _require_windows()
    assert _user32 is not None
    process_id = wintypes.DWORD()
    _user32.GetWindowThreadProcessId(int(hwnd), ctypes.byref(process_id))
    if not process_id.value and not is_window(hwnd):
        raise LookupError(f"Window no longer exists: {int(hwnd)}")
    return int(process_id.value)


def get_foreground_window() -> HWND | None:
    _require_windows()
    assert _user32 is not None
    hwnd = _user32.GetForegroundWindow()
    return HWND(hwnd) if hwnd else None


def set_foreground_window(hwnd: HWND | int) -> bool:
    _require_windows()
    assert _user32 is not None
    return bool(_user32.SetForegroundWindow(int(hwnd)))


def show_window(hwnd: HWND | int, command: int) -> None:
    _require_windows()
    assert _user32 is not None
    # ShowWindow returns the previous visibility state, not success/failure.
    _user32.ShowWindow(int(hwnd), command)


def restore_window(hwnd: HWND | int) -> None:
    show_window(hwnd, SW_RESTORE)


def minimize_window(hwnd: HWND | int) -> None:
    show_window(hwnd, SW_MINIMIZE)


def maximize_window(hwnd: HWND | int) -> None:
    show_window(hwnd, SW_MAXIMIZE)


def move_window(hwnd: HWND | int, x: int, y: int) -> None:
    _require_windows()
    assert _user32 is not None
    ctypes.set_last_error(0)
    if not _user32.SetWindowPos(
        int(hwnd), 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER
    ):
        raise _last_error("SetWindowPos")


def resize_window(hwnd: HWND | int, width: int, height: int) -> None:
    if width <= 0 or height <= 0:
        raise ValueError("Window width and height must be positive")
    _require_windows()
    assert _user32 is not None
    ctypes.set_last_error(0)
    if not _user32.SetWindowPos(
        int(hwnd), 0, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER
    ):
        raise _last_error("SetWindowPos")


def close_window(hwnd: HWND | int) -> None:
    _require_windows()
    assert _user32 is not None
    ctypes.set_last_error(0)
    if not _user32.PostMessageW(int(hwnd), WM_CLOSE, 0, 0):
        raise _last_error("PostMessageW")


def get_system_metric(index: int) -> int:
    _require_windows()
    assert _user32 is not None
    return int(_user32.GetSystemMetrics(index))


def get_double_click_time() -> float:
    _require_windows()
    assert _user32 is not None
    return float(_user32.GetDoubleClickTime()) / 1000.0


def send_input(inputs: Iterable[INPUT]) -> int:
    _require_windows()
    assert _user32 is not None
    values = tuple(inputs)
    if not values:
        return 0
    array = (INPUT * len(values))(*values)
    ctypes.set_last_error(0)
    sent = int(_user32.SendInput(len(array), array, ctypes.sizeof(INPUT)))
    if sent != len(array):
        raise _last_error(f"SendInput accepted {sent} of {len(array)} events")
    return sent


def capture_screen(rect: Rect) -> bytes:
    """Capture a physical-pixel screen rectangle as tightly packed BGRA bytes."""

    if rect.width <= 0 or rect.height <= 0:
        raise ValueError(f"Cannot capture an empty rectangle: {rect}")
    _require_windows()
    assert _user32 is not None and _gdi32 is not None

    screen = _user32.GetDC(None)
    if not screen:
        raise _last_error("GetDC")
    memory = None
    bitmap = None
    previous = None
    try:
        memory = _gdi32.CreateCompatibleDC(screen)
        if not memory:
            raise _last_error("CreateCompatibleDC")

        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = rect.width
        info.bmiHeader.biHeight = -rect.height
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = BI_RGB

        pixels = ctypes.c_void_p()
        bitmap = _gdi32.CreateDIBSection(
            screen,
            ctypes.byref(info),
            DIB_RGB_COLORS,
            ctypes.byref(pixels),
            None,
            0,
        )
        if not bitmap or not pixels.value:
            raise _last_error("CreateDIBSection")

        previous = _gdi32.SelectObject(memory, bitmap)
        if not previous:
            raise _last_error("SelectObject")

        if not _gdi32.BitBlt(
            memory,
            0,
            0,
            rect.width,
            rect.height,
            screen,
            rect.left,
            rect.top,
            SRCCOPY | CAPTUREBLT,
        ):
            raise _last_error("BitBlt")

        return ctypes.string_at(pixels.value, rect.width * rect.height * 4)
    finally:
        if previous and memory:
            _gdi32.SelectObject(memory, previous)
        if bitmap:
            _gdi32.DeleteObject(bitmap)
        if memory:
            _gdi32.DeleteDC(memory)
        _user32.ReleaseDC(None, screen)
