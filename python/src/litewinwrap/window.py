from __future__ import annotations

import re
import time
from dataclasses import dataclass
from typing import Pattern

from . import win32
from .mouse import Button
from .types import Capture, HWND, Match, Rect, Target


_POLL_INTERVAL = 0.05
TextSelector = str | Pattern[str]


class WindowNotFoundError(LookupError):
    pass


class WindowAmbiguousError(LookupError):
    def __init__(self, windows: tuple[Window, ...]):
        self.windows = windows
        super().__init__(f"Expected one window, found {len(windows)}")


class FocusTimeoutError(TimeoutError):
    pass


class WindowCloseTimeoutError(TimeoutError):
    pass


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
class Window:
    """A live reference to a Win32 window. Only the HWND is retained."""

    hwnd: HWND

    @property
    def exists(self) -> bool:
        return win32.is_window(self.hwnd)

    @property
    def title(self) -> str:
        return win32.get_window_text(self.hwnd)

    @property
    def class_name(self) -> str:
        return win32.get_class_name(self.hwnd)

    @property
    def rect(self) -> Rect:
        return win32.get_window_rect(self.hwnd)

    @property
    def process_id(self) -> int:
        return win32.get_window_process_id(self.hwnd)

    @property
    def parent(self) -> Window | None:
        parent = win32.get_parent(self.hwnd)
        return Window(parent) if parent is not None else None

    @property
    def visible(self) -> bool:
        return win32.is_window_visible(self.hwnd)

    @property
    def enabled(self) -> bool:
        return win32.is_window_enabled(self.hwnd)

    @property
    def minimized(self) -> bool:
        return win32.is_iconic(self.hwnd)

    @property
    def foreground(self) -> bool:
        return win32.get_foreground_window() == self.hwnd

    @classmethod
    def list(cls, *, visible_only: bool = True) -> tuple[Window, ...]:
        windows = tuple(cls(hwnd) for hwnd in win32.enum_windows())
        if visible_only:
            windows = tuple(window for window in windows if window.visible)
        return windows

    @classmethod
    def find_all(
        cls,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        process_id: int | None = None,
        visible_only: bool = True,
    ) -> tuple[Window, ...]:
        return tuple(
            window
            for window in cls.list(visible_only=visible_only)
            if (title is None or _matches(window.title, title))
            and (class_name is None or _matches(window.class_name, class_name))
            and (process_id is None or window.process_id == process_id)
        )

    @classmethod
    def find(
        cls,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        process_id: int | None = None,
        visible_only: bool = True,
        timeout: float = 0.0,
    ) -> Window:
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            matches = cls.find_all(
                title,
                class_name=class_name,
                process_id=process_id,
                visible_only=visible_only,
            )
            if len(matches) == 1:
                return matches[0]
            if len(matches) > 1:
                raise WindowAmbiguousError(matches)

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                selectors = _describe_selectors(title, class_name, process_id)
                raise WindowNotFoundError(
                    f"No window matched {selectors} within {timeout:.3f}s"
                )
            time.sleep(min(_POLL_INTERVAL, remaining))

    def get_children(
        self,
        *,
        recursive: bool = True,
        visible_only: bool = True,
    ) -> tuple[Window, ...]:
        handles = win32.enum_child_windows(self.hwnd)
        if not recursive:
            handles = tuple(
                hwnd for hwnd in handles if win32.get_parent(hwnd) == self.hwnd
            )
        windows = tuple(Window(hwnd) for hwnd in handles)
        if visible_only:
            windows = tuple(window for window in windows if window.visible)
        return windows

    def find_children(
        self,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        recursive: bool = True,
        visible_only: bool = True,
        timeout: float = 0.0,
    ) -> tuple[Window, ...]:
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            matches = tuple(
                window
                for window in self.get_children(
                    recursive=recursive,
                    visible_only=visible_only,
                )
                if (title is None or _matches(window.title, title))
                and (class_name is None or _matches(window.class_name, class_name))
            )
            if matches:
                return matches

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return ()
            time.sleep(min(_POLL_INTERVAL, remaining))

    def find_child(
        self,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        recursive: bool = True,
        visible_only: bool = True,
        timeout: float = 0.0,
    ) -> Window:
        matches = self.find_children(
            title,
            class_name=class_name,
            recursive=recursive,
            visible_only=visible_only,
            timeout=timeout,
        )
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise WindowAmbiguousError(matches)
        selectors = _describe_selectors(title, class_name)
        raise WindowNotFoundError(
            f"No child of {int(self.hwnd)} matched {selectors} within {timeout:.3f}s"
        )

    def screenshot(self) -> Capture:
        from . import match as image_match

        return image_match.capture(self.hwnd)

    def find_targets(
        self,
        target: Target,
        *,
        threshold: float = 0.90,
        timeout: float = 0.0,
        overlap: float = 0.30,
    ) -> tuple[Match, ...]:
        from . import match as image_match

        return image_match.find_all(
            self.hwnd,
            target,
            threshold=threshold,
            timeout=timeout,
            overlap=overlap,
        )

    def find_target(
        self,
        target: Target,
        *,
        threshold: float = 0.90,
        timeout: float = 0.0,
        overlap: float = 0.30,
        retry_on_ambiguity: bool = False,
    ) -> Match:
        from . import match as image_match

        return image_match.find(
            self.hwnd,
            target,
            threshold=threshold,
            timeout=timeout,
            overlap=overlap,
            retry_on_ambiguity=retry_on_ambiguity,
        )

    def find_best_target(
        self,
        target: Target,
        *,
        threshold: float = 0.90,
        timeout: float = 0.0,
        overlap: float = 0.30,
    ) -> Match:
        from . import match as image_match

        return image_match.find_best(
            self.hwnd,
            target,
            threshold=threshold,
            timeout=timeout,
            overlap=overlap,
        )

    def click_target(
        self,
        target: Target,
        *,
        threshold: float = 0.90,
        timeout: float = 0.0,
        overlap: float = 0.30,
        retry_on_ambiguity: bool = False,
        button: Button = "left",
    ) -> Match:
        from . import match as image_match

        return image_match.click(
            self.hwnd,
            target,
            threshold=threshold,
            timeout=timeout,
            overlap=overlap,
            retry_on_ambiguity=retry_on_ambiguity,
            button=button,
        )

    def click_best_target(
        self,
        target: Target,
        *,
        threshold: float = 0.90,
        timeout: float = 0.0,
        overlap: float = 0.30,
        button: Button = "left",
    ) -> Match:
        from . import match as image_match

        return image_match.click_best(
            self.hwnd,
            target,
            threshold=threshold,
            timeout=timeout,
            overlap=overlap,
            button=button,
        )

    def focus(self, *, restore: bool = True, timeout: float = 1.0) -> Window:
        if not self.exists:
            raise LookupError(f"Window no longer exists: {int(self.hwnd)}")
        if restore and self.minimized:
            win32.restore_window(self.hwnd)

        win32.set_foreground_window(self.hwnd)
        deadline = time.monotonic() + max(0.0, timeout)
        while not self.foreground:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                actual = win32.get_foreground_window()
                raise FocusTimeoutError(
                    f"Could not focus {int(self.hwnd)}; foreground is {actual}"
                )
            time.sleep(min(_POLL_INTERVAL, remaining))
        return self

    def move(self, x: int, y: int) -> Rect:
        win32.move_window(self.hwnd, x, y)
        return self.rect

    def resize(self, width: int, height: int) -> Rect:
        win32.resize_window(self.hwnd, width, height)
        return self.rect

    def restore(self) -> Rect:
        win32.restore_window(self.hwnd)
        return self.rect

    def minimize(self) -> Window:
        win32.minimize_window(self.hwnd)
        return self

    def maximize(self) -> Rect:
        win32.maximize_window(self.hwnd)
        return self.rect

    def close(self, *, timeout: float = 0.0) -> None:
        win32.close_window(self.hwnd)
        if timeout <= 0:
            return

        deadline = time.monotonic() + timeout
        while self.exists:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise WindowCloseTimeoutError(
                    f"Window {int(self.hwnd)} did not close within {timeout:.3f}s"
                )
            time.sleep(min(_POLL_INTERVAL, remaining))

    def __int__(self) -> int:
        return int(self.hwnd)
