from . import keyboard, match, mouse, win32
from .goldens import Goldens, GoldensFormatError
from .types import Capture, HWND, Match, Point, Rect, Target
from .window import (
    FocusTimeoutError,
    Window,
    WindowAmbiguousError,
    WindowCloseTimeoutError,
    WindowNotFoundError,
)


__all__ = [
    "Capture",
    "FocusTimeoutError",
    "Goldens",
    "GoldensFormatError",
    "HWND",
    "Match",
    "Point",
    "Rect",
    "Target",
    "Window",
    "WindowAmbiguousError",
    "WindowCloseTimeoutError",
    "WindowNotFoundError",
    "keyboard",
    "match",
    "mouse",
    "win32",
]

__version__ = "0.1.0"
