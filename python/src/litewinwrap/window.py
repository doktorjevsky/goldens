from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from . import keyboard, win32
from .automation import TextSelector, _describe_selectors, _matches
from .mouse import Button
from .types import Capture, HWND, Match, Rect, Target


if TYPE_CHECKING:
    from .automation import Automation
    from .keyboard import Key


_POLL_INTERVAL_SECONDS = 0.05


class WindowNotFoundError(LookupError):
    pass


class WindowAmbiguousError(LookupError):
    def __init__(self, windows: tuple[Window, ...]) -> None:
        self.windows = windows
        super().__init__(f"Expected one window, found {len(windows)}")


class FocusTimeoutError(TimeoutError):
    pass


class WindowCloseTimeoutError(TimeoutError):
    pass


@dataclass(frozen=True, slots=True)
class Window:
    """A live HWND bound to one :class:`Automation` policy."""

    hwnd: HWND
    automation: Automation = field(repr=False, compare=False)

    def _window(self, hwnd: HWND | int) -> Window:
        return type(self)(HWND(int(hwnd)), self.automation)

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
        return self._window(parent) if parent is not None else None

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

    def children(
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
        windows = tuple(self._window(hwnd) for hwnd in handles)
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
        timeout_seconds: float | None = None,
    ) -> tuple[Window, ...]:
        timeout_seconds = self.automation._resolve_timeout_seconds(timeout_seconds)
        deadline_seconds = time.monotonic() + timeout_seconds
        while True:
            matches = tuple(
                window
                for window in self.children(
                    recursive=recursive,
                    visible_only=visible_only,
                )
                if (title is None or _matches(window.title, title))
                and (class_name is None or _matches(window.class_name, class_name))
            )
            if matches:
                return matches

            remaining_seconds = deadline_seconds - time.monotonic()
            if remaining_seconds <= 0:
                return ()
            time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))

    def find_child(
        self,
        title: TextSelector | None = None,
        *,
        class_name: TextSelector | None = None,
        recursive: bool = True,
        visible_only: bool = True,
        timeout_seconds: float | None = None,
    ) -> Window:
        timeout_seconds = self.automation._resolve_timeout_seconds(timeout_seconds)
        matches = self.find_children(
            title,
            class_name=class_name,
            recursive=recursive,
            visible_only=visible_only,
            timeout_seconds=timeout_seconds,
        )
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise WindowAmbiguousError(matches)
        selectors = _describe_selectors(title, class_name)
        raise WindowNotFoundError(
            f"No child of {int(self.hwnd)} matched {selectors} within "
            f"{timeout_seconds:.3f}s"
        )

    def capture(self) -> Capture:
        from . import match as image_match

        return image_match.capture(self.hwnd)

    def locate_all(
        self,
        target: Target,
        *,
        threshold: float | None = None,
        timeout_seconds: float | None = None,
        overlap: float | None = None,
    ) -> tuple[Match, ...]:
        from . import match as image_match

        return image_match.find_all(
            self.hwnd,
            target,
            threshold=self.automation._resolve_threshold(threshold),
            timeout_seconds=self.automation._resolve_timeout_seconds(timeout_seconds),
            overlap=self.automation._resolve_overlap(overlap),
        )

    def locate(
        self,
        target: Target,
        *,
        threshold: float | None = None,
        timeout_seconds: float | None = None,
        overlap: float | None = None,
        retry_on_ambiguity: bool | None = None,
    ) -> Match:
        from . import match as image_match

        retry = (
            self.automation.retry_on_ambiguity
            if retry_on_ambiguity is None
            else retry_on_ambiguity
        )
        return image_match.find(
            self.hwnd,
            target,
            threshold=self.automation._resolve_threshold(threshold),
            timeout_seconds=self.automation._resolve_timeout_seconds(timeout_seconds),
            overlap=self.automation._resolve_overlap(overlap),
            retry_on_ambiguity=retry,
        )

    def locate_best(
        self,
        target: Target,
        *,
        threshold: float | None = None,
        timeout_seconds: float | None = None,
        overlap: float | None = None,
    ) -> Match:
        from . import match as image_match

        return image_match.find_best(
            self.hwnd,
            target,
            threshold=self.automation._resolve_threshold(threshold),
            timeout_seconds=self.automation._resolve_timeout_seconds(timeout_seconds),
            overlap=self.automation._resolve_overlap(overlap),
        )

    def click(
        self,
        target: Target,
        *,
        threshold: float | None = None,
        timeout_seconds: float | None = None,
        overlap: float | None = None,
        retry_on_ambiguity: bool | None = None,
        button: Button = "left",
        focus: bool | None = None,
        settle_seconds: float | None = None,
    ) -> Match:
        from . import match as image_match

        if self.automation._resolve_focus(focus) and not self.foreground:
            self.focus(settle_seconds=0.0)
        retry = (
            self.automation.retry_on_ambiguity
            if retry_on_ambiguity is None
            else retry_on_ambiguity
        )
        return image_match.click(
            self.hwnd,
            target,
            threshold=self.automation._resolve_threshold(threshold),
            timeout_seconds=self.automation._resolve_timeout_seconds(timeout_seconds),
            overlap=self.automation._resolve_overlap(overlap),
            retry_on_ambiguity=retry,
            button=button,
            wait_after_seconds=self.automation._resolve_settle_seconds(settle_seconds),
        )

    def click_best(
        self,
        target: Target,
        *,
        threshold: float | None = None,
        timeout_seconds: float | None = None,
        overlap: float | None = None,
        button: Button = "left",
        focus: bool | None = None,
        settle_seconds: float | None = None,
    ) -> Match:
        from . import match as image_match

        if self.automation._resolve_focus(focus) and not self.foreground:
            self.focus(settle_seconds=0.0)
        return image_match.click_best(
            self.hwnd,
            target,
            threshold=self.automation._resolve_threshold(threshold),
            timeout_seconds=self.automation._resolve_timeout_seconds(timeout_seconds),
            overlap=self.automation._resolve_overlap(overlap),
            button=button,
            wait_after_seconds=self.automation._resolve_settle_seconds(settle_seconds),
        )

    def type_text(
        self,
        text: str,
        *,
        interval_seconds: float = 0.0,
        chunk_size: int = 256,
        focus: bool | None = None,
        settle_seconds: float | None = None,
    ) -> Window:
        if self.automation._resolve_focus(focus) and not self.foreground:
            self.focus(settle_seconds=0.0)
        keyboard.type_text(
            text,
            interval_seconds=interval_seconds,
            chunk_size=chunk_size,
        )
        self.automation._settle(settle_seconds)
        return self

    def press(
        self,
        *keys: Key,
        count: int = 1,
        interval_seconds: float = 0.0,
        focus: bool | None = None,
        settle_seconds: float | None = None,
    ) -> Window:
        if self.automation._resolve_focus(focus) and not self.foreground:
            self.focus(settle_seconds=0.0)
        keyboard.press(*keys, count=count, interval_seconds=interval_seconds)
        self.automation._settle(settle_seconds)
        return self

    def focus(
        self,
        *,
        restore: bool = True,
        timeout_seconds: float | None = None,
        settle_seconds: float | None = None,
    ) -> Window:
        if not self.exists:
            raise LookupError(f"Window no longer exists: {int(self.hwnd)}")
        if restore and self.minimized:
            win32.restore_window(self.hwnd)

        win32.set_foreground_window(self.hwnd)
        timeout_seconds = self.automation._resolve_timeout_seconds(timeout_seconds)
        deadline_seconds = time.monotonic() + timeout_seconds
        while not self.foreground:
            remaining_seconds = deadline_seconds - time.monotonic()
            if remaining_seconds <= 0:
                actual = win32.get_foreground_window()
                raise FocusTimeoutError(
                    f"Could not focus {int(self.hwnd)}; foreground is {actual}"
                )
            time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))
        self.automation._settle(settle_seconds)
        return self

    def move(self, x: int, y: int, *, settle_seconds: float | None = None) -> Rect:
        win32.move_window(self.hwnd, x, y)
        self.automation._settle(settle_seconds)
        return self.rect

    def resize(
        self,
        width: int,
        height: int,
        *,
        settle_seconds: float | None = None,
    ) -> Rect:
        win32.resize_window(self.hwnd, width, height)
        self.automation._settle(settle_seconds)
        return self.rect

    def restore(self, *, settle_seconds: float | None = None) -> Rect:
        win32.restore_window(self.hwnd)
        self.automation._settle(settle_seconds)
        return self.rect

    def minimize(self, *, settle_seconds: float | None = None) -> Window:
        win32.minimize_window(self.hwnd)
        self.automation._settle(settle_seconds)
        return self

    def maximize(self, *, settle_seconds: float | None = None) -> Rect:
        win32.maximize_window(self.hwnd)
        self.automation._settle(settle_seconds)
        return self.rect

    def close(self, *, timeout_seconds: float | None = None) -> None:
        win32.close_window(self.hwnd)
        timeout_seconds = self.automation._resolve_timeout_seconds(timeout_seconds)
        if timeout_seconds == 0.0:
            return
        deadline_seconds = time.monotonic() + timeout_seconds
        while self.exists:
            remaining_seconds = deadline_seconds - time.monotonic()
            if remaining_seconds <= 0:
                raise WindowCloseTimeoutError(
                    f"Window {int(self.hwnd)} did not close within "
                    f"{timeout_seconds:.3f}s"
                )
            time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))

    def __int__(self) -> int:
        return int(self.hwnd)
