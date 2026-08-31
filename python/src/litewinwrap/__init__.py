from . import keyboard, match, mouse, reporting, win32
from .automation import Automation
from .goldens import Goldens, GoldensFormatError
from .match import TargetAmbiguousError, TargetNotFoundError
from .reporting import Reports
from .types import Capture, HWND, Match, Point, Rect, Target
from .window import (
    FocusTimeoutError,
    Window,
    WindowAmbiguousError,
    WindowCloseTimeoutError,
    WindowNotFoundError,
)


__all__ = [
    "__version__",
    "Automation",
    "Capture",
    "FocusTimeoutError",
    "Goldens",
    "GoldensFormatError",
    "HWND",
    "Match",
    "Point",
    "Rect",
    "Reports",
    "Target",
    "TargetAmbiguousError",
    "TargetNotFoundError",
    "Window",
    "WindowAmbiguousError",
    "WindowCloseTimeoutError",
    "WindowNotFoundError",
    "keyboard",
    "match",
    "mouse",
    "reporting",
    "win32",
]

__version__ = "0.1.0a9"
