from __future__ import annotations

import math
import time
from collections.abc import Iterator
from contextlib import contextmanager
from typing import Literal

from . import win32
from .types import Point


Button = Literal["left", "right", "middle", "x1", "x2"]

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_XDOWN = 0x0080
MOUSEEVENTF_XUP = 0x0100
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
MOUSEEVENTF_MOVE_NOCOALESCE = 0x2000
MOUSEEVENTF_VIRTUALDESK = 0x4000
MOUSEEVENTF_ABSOLUTE = 0x8000

WHEEL_DELTA = 120
XBUTTON1 = 0x0001
XBUTTON2 = 0x0002

_MOVE_INTERVAL_SECONDS = 1.0 / 60.0
_DEFAULT_DRAG_DURATION_SECONDS = 0.25

_DOWN = {
    "left": (MOUSEEVENTF_LEFTDOWN, 0),
    "right": (MOUSEEVENTF_RIGHTDOWN, 0),
    "middle": (MOUSEEVENTF_MIDDLEDOWN, 0),
    "x1": (MOUSEEVENTF_XDOWN, XBUTTON1),
    "x2": (MOUSEEVENTF_XDOWN, XBUTTON2),
}
_UP = {
    "left": (MOUSEEVENTF_LEFTUP, 0),
    "right": (MOUSEEVENTF_RIGHTUP, 0),
    "middle": (MOUSEEVENTF_MIDDLEUP, 0),
    "x1": (MOUSEEVENTF_XUP, XBUTTON1),
    "x2": (MOUSEEVENTF_XUP, XBUTTON2),
}


def _input(flags: int, *, dx: int = 0, dy: int = 0, data: int = 0) -> win32.INPUT:
    value = win32.INPUT()
    value.type = win32.INPUT_MOUSE
    value.mi = win32.MOUSEINPUT(
        dx=dx,
        dy=dy,
        mouseData=data & 0xFFFFFFFF,
        dwFlags=flags,
        time_milliseconds=0,
        dwExtraInfo=0,
    )
    return value


def _validate_duration_seconds(duration_seconds: float) -> None:
    if not math.isfinite(duration_seconds) or duration_seconds < 0:
        raise ValueError("Movement duration must be a finite non-negative number")


def _absolute_move_input(
    point: Point | tuple[int, int],
    *,
    virtual_screen: tuple[int, int, int, int],
) -> win32.INPUT:
    x, y = point
    left, top, width, height = virtual_screen
    if width <= 1 or height <= 1:
        raise OSError("Windows reported an invalid virtual-screen size")
    if not (left <= x < left + width and top <= y < top + height):
        raise ValueError(f"Point ({x}, {y}) is outside the virtual screen")

    normalized_x = round((x - left) * 65535 / (width - 1))
    normalized_y = round((y - top) * 65535 / (height - 1))
    return _input(
        MOUSEEVENTF_MOVE
        | MOUSEEVENTF_MOVE_NOCOALESCE
        | MOUSEEVENTF_ABSOLUTE
        | MOUSEEVENTF_VIRTUALDESK,
        dx=normalized_x,
        dy=normalized_y,
    )


def move_to(
    point: Point | tuple[int, int],
    *,
    duration_seconds: float = 0.0,
) -> int:
    """Move to an absolute screen point, optionally over a human-like duration."""

    _validate_duration_seconds(duration_seconds)
    destination = Point(*point)
    virtual_screen = (
        win32.get_system_metric(win32.SM_XVIRTUALSCREEN),
        win32.get_system_metric(win32.SM_YVIRTUALSCREEN),
        win32.get_system_metric(win32.SM_CXVIRTUALSCREEN),
        win32.get_system_metric(win32.SM_CYVIRTUALSCREEN),
    )
    destination_input = _absolute_move_input(
        destination,
        virtual_screen=virtual_screen,
    )
    if duration_seconds == 0:
        return win32.send_input([destination_input])

    origin = win32.get_cursor_position()
    steps = max(1, math.ceil(duration_seconds / _MOVE_INTERVAL_SECONDS))
    step_seconds = duration_seconds / steps
    sent = 0
    for step in range(1, steps + 1):
        time.sleep(step_seconds)
        progress = step / steps
        intermediate = Point(
            round(origin.x + (destination.x - origin.x) * progress),
            round(origin.y + (destination.y - origin.y) * progress),
        )
        sent += win32.send_input(
            [
                _absolute_move_input(
                    intermediate,
                    virtual_screen=virtual_screen,
                )
            ]
        )
    return sent


def move_by(dx: int, dy: int, *, duration_seconds: float = 0.0) -> int:
    """Move by an exact number of screen pixels."""

    _validate_duration_seconds(duration_seconds)
    origin = win32.get_cursor_position()
    return move_to(
        Point(origin.x + dx, origin.y + dy),
        duration_seconds=duration_seconds,
    )


def button_down(button: Button = "left") -> int:
    flags, data = _DOWN[button]
    return win32.send_input([_input(flags, data=data)])


def button_up(button: Button = "left") -> int:
    flags, data = _UP[button]
    return win32.send_input([_input(flags, data=data)])


@contextmanager
def hold(button: Button = "left") -> Iterator[None]:
    """Hold a mouse button for the duration of a ``with`` block."""

    button_down(button)
    try:
        yield
    finally:
        button_up(button)


def click(
    point: Point | tuple[int, int] | None = None,
    *,
    button: Button = "left",
    count: int = 1,
    interval_seconds: float | None = None,
) -> int:
    if count <= 0:
        raise ValueError("Click count must be positive")
    sent = move_to(point) if point is not None else 0
    down_flags, down_data = _DOWN[button]
    up_flags, up_data = _UP[button]
    if interval_seconds is None:
        interval_seconds = win32.get_double_click_seconds() * 0.5

    for index in range(count):
        sent += win32.send_input(
            [
                _input(down_flags, data=down_data),
                _input(up_flags, data=up_data),
            ]
        )
        if index + 1 < count:
            time.sleep(max(0.0, interval_seconds))
    return sent


def double_click(
    point: Point | tuple[int, int] | None = None,
    *,
    button: Button = "left",
) -> int:
    return click(point, button=button, count=2)


def scroll(notches: int, *, horizontal: bool = False) -> int:
    flag = MOUSEEVENTF_HWHEEL if horizontal else MOUSEEVENTF_WHEEL
    return win32.send_input([_input(flag, data=notches * WHEEL_DELTA)])


def drag_to(
    destination: Point | tuple[int, int],
    *,
    origin: Point | tuple[int, int] | None = None,
    button: Button = "left",
    duration_seconds: float = _DEFAULT_DRAG_DURATION_SECONDS,
) -> int:
    _validate_duration_seconds(duration_seconds)
    sent = move_to(origin) if origin is not None else 0
    sent += button_down(button)
    try:
        sent += move_to(destination, duration_seconds=duration_seconds)
    finally:
        sent += button_up(button)
    return sent


def drag_by(
    dx: int,
    dy: int,
    *,
    button: Button = "left",
    duration_seconds: float = _DEFAULT_DRAG_DURATION_SECONDS,
) -> int:
    _validate_duration_seconds(duration_seconds)
    sent = button_down(button)
    try:
        sent += move_by(dx, dy, duration_seconds=duration_seconds)
    finally:
        sent += button_up(button)
    return sent
