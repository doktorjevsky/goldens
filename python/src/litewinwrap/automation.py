from __future__ import annotations

import re
import sys
import time
from dataclasses import dataclass
from math import isfinite
from typing import TYPE_CHECKING, Literal, Pattern

from . import win32
from .types import HWND


if TYPE_CHECKING:
    from .window import Window


_POLL_INTERVAL_SECONDS = 0.05
TextSelector = str | Pattern[str]


def _duration_seconds(value_seconds: float, name: str) -> float:
    result_seconds = float(value_seconds)
    if not isfinite(result_seconds) or result_seconds < 0.0:
        raise ValueError(f"{name} must be a finite non-negative number")
    return result_seconds


def _matches(value: str, selector: TextSelector | None) -> bool:
    if selector is None:
        return True
    if isinstance(selector, re.Pattern):
        return selector.search(value) is not None
    return value == selector


def _describe_selectors(
    title: TextSelector | None,
    class_name: TextSelector | None,
    process_id: int | None = None,
) -> str:
    values = []
    if title is not None:
        values.append(f"title={title!r}")
    if class_name is not None:
        values.append(f"class_name={class_name!r}")
    if process_id is not None:
        values.append(f"process_id={process_id}")
    return ", ".join(values) or "the supplied selectors"


@dataclass(frozen=True, slots=True)
class Automation:
    """Policy and entry point for one desktop-automation workflow."""

    timeout_seconds: float = 2.0
    settle_seconds: float = 0.15
    threshold: float = 0.90
    overlap: float = 0.30
    retry_on_ambiguity: bool = False
    focus_before_input: bool = True
    dpi_awareness: Literal["per-monitor-v2", "unchanged"] = "per-monitor-v2"

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "timeout_seconds",
            _duration_seconds(self.timeout_seconds, "timeout_seconds"),
        )
        object.__setattr__(
            self,
            "settle_seconds",
            _duration_seconds(self.settle_seconds, "settle_seconds"),
        )
        if not 0.0 <= self.threshold <= 1.0:
            raise ValueError("Match threshold must be between 0 and 1")
        if not 0.0 <= self.overlap < 1.0:
            raise ValueError("Overlap threshold must be at least 0 and less than 1")
        if not isinstance(self.focus_before_input, bool):
            raise TypeError("Focus-before-input policy must be a boolean")
        if self.dpi_awareness not in ("per-monitor-v2", "unchanged"):
            raise ValueError("DPI awareness must be 'per-monitor-v2' or 'unchanged'")
        if sys.platform == "win32" and self.dpi_awareness == "per-monitor-v2":
            win32.enable_per_monitor_dpi_awareness()

    def _resolve_timeout_seconds(self, timeout_seconds: float | None) -> float:
        return (
            self.timeout_seconds
            if timeout_seconds is None
            else _duration_seconds(timeout_seconds, "timeout_seconds")
        )

    def _resolve_settle_seconds(self, settle_seconds: float | None) -> float:
        return (
            self.settle_seconds
            if settle_seconds is None
            else _duration_seconds(settle_seconds, "settle_seconds")
        )

    def _resolve_threshold(self, threshold: float | None) -> float:
        result = self.threshold if threshold is None else float(threshold)
        if not 0.0 <= result <= 1.0:
            raise ValueError("Match threshold must be between 0 and 1")
        return result

    def _resolve_overlap(self, overlap: float | None) -> float:
        result = self.overlap if overlap is None else float(overlap)
        if not 0.0 <= result < 1.0:
            raise ValueError("Overlap threshold must be at least 0 and less than 1")
        return result

    def _resolve_focus(self, focus: bool | None) -> bool:
        if focus is None:
            return self.focus_before_input
        if not isinstance(focus, bool):
            raise TypeError("Focus override must be a boolean")
        return focus

    def _settle(self, settle_seconds: float | None = None) -> None:
        delay_seconds = self._resolve_settle_seconds(settle_seconds)
        if delay_seconds:
            time.sleep(delay_seconds)

    def window(self, hwnd: HWND | int) -> Window:
        """Bind an existing HWND to this automation policy."""

        from .window import Window

        return Window(HWND(int(hwnd)), self)

    def windows(self, *, visible_only: bool = True) -> tuple[Window, ...]:
        """Return the current top-level windows."""

        windows = tuple(self.window(hwnd) for hwnd in win32.enum_windows())
        if visible_only:
            windows = tuple(window for window in windows if window.visible)
        return windows

    def find_windows(
        self,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        process_id: int | None = None,
        visible_only: bool = True,
    ) -> tuple[Window, ...]:
        """Return every top-level window matching the supplied selectors."""

        return tuple(
            window
            for window in self.windows(visible_only=visible_only)
            if (title is None or _matches(window.title, title))
            and (class_name is None or _matches(window.class_name, class_name))
            and (process_id is None or window.process_id == process_id)
        )

    def find_window(
        self,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        process_id: int | None = None,
        visible_only: bool = True,
        timeout_seconds: float | None = None,
    ) -> Window:
        """Wait for exactly one top-level window matching the selectors."""

        from .window import WindowAmbiguousError, WindowNotFoundError

        timeout_seconds = self._resolve_timeout_seconds(timeout_seconds)
        deadline_seconds = time.monotonic() + timeout_seconds
        while True:
            matches = self.find_windows(
                title,
                class_name=class_name,
                process_id=process_id,
                visible_only=visible_only,
            )
            if len(matches) == 1:
                return matches[0]
            if len(matches) > 1:
                raise WindowAmbiguousError(matches)

            remaining_seconds = deadline_seconds - time.monotonic()
            if remaining_seconds <= 0:
                selectors = _describe_selectors(title, class_name, process_id)
                raise WindowNotFoundError(
                    f"No window matched {selectors} within {timeout_seconds:.3f}s"
                )
            time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))
