from . import keyboard, match, mouse, ocr, reporting, win32
from .automation import Automation
from .display import Display, DisplayScaleRestartRequired
from .goldens import Goldens, GoldensFormatError, InconsistentGoldenScaleError
from .match import TargetAmbiguousError, TargetNotFoundError
from .reporting import Reports
from .types import Capture, HWND, Match, Point, Rect, Target, TextMatch
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
    "Display",
    "DisplayScaleRestartRequired",
    "FocusTimeoutError",
    "Goldens",
    "GoldensFormatError",
    "InconsistentGoldenScaleError",
    "HWND",
    "Match",
    "Point",
    "Rect",
    "Reports",
    "Target",
    "TargetAmbiguousError",
    "TargetNotFoundError",
    "TextMatch",
    "Window",
    "WindowAmbiguousError",
    "WindowCloseTimeoutError",
    "WindowNotFoundError",
    "keyboard",
    "match",
    "mouse",
    "ocr",
    "reporting",
    "win32",
]

__version__ = "0.1.0a10+ocr"
