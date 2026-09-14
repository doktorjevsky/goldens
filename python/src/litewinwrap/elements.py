from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Literal

from . import _uia, reporting
from .automation import TextSelector, _matches
from .types import HWND, Rect

if TYPE_CHECKING:
    from .window import Window

from ._uia import (
    ElementReadOnlyError,
    ElementUnavailableError,
    ElementUnsupportedError,
    UIAutomationError,
)

__all__ = [
    "Element",
    "ElementInfo",
    "ElementAmbiguousError",
    "ElementNotFoundError",
    "ElementReadOnlyError",
    "ElementTimeoutError",
    "ElementUnavailableError",
    "ElementUnsupportedError",
    "UIAutomationError",
]

_POLL_INTERVAL_SECONDS = 0.05


class ElementNotFoundError(LookupError):
    pass


class ElementAmbiguousError(LookupError):
    def __init__(self, elements: tuple[Element, ...]) -> None:
        self.elements = elements
        super().__init__(f"Expected one element, found {len(elements)}")


class ElementTimeoutError(TimeoutError):
    pass


@dataclass(frozen=True, slots=True)
class ElementInfo:
    """A diagnostic snapshot. Values are omitted, including password contents."""

    name: str
    automation_id: str
    control_type: str
    class_name: str
    framework_id: str
    process_id: int
    hwnd: HWND | None
    enabled: bool
    offscreen: bool
    is_password: bool
    rect: Rect
    patterns: tuple[str, ...]


@dataclass(frozen=True, slots=True, repr=False)
class Element:
    """A live UI Automation control bound to a Window and its policy.

    Controls need not have an HWND. ``hwnd`` is the owning window's handle,
    used by failure reporting; ``native_hwnd`` is the control's own handle.
    """

    window: Window = field(repr=False, compare=False)
    _native: _uia.NativeElement = field(repr=False)

    @property
    def hwnd(self) -> HWND:
        return self.window.hwnd

    @property
    def native_hwnd(self) -> HWND | None:
        value = self._native.property("hwnd")
        return HWND(value) if value else None

    @property
    def name(self) -> str:
        return self._native.property("name")

    @property
    def automation_id(self) -> str:
        return self._native.property("automation_id")

    @property
    def control_type(self) -> str:
        return self._native.property("control_type")

    @property
    def class_name(self) -> str:
        return self._native.property("class_name")

    @property
    def rect(self) -> Rect:
        return Rect(*self._native.property("rect"))

    @property
    def enabled(self) -> bool:
        return self._native.property("enabled")

    @property
    def offscreen(self) -> bool:
        return self._native.property("offscreen")

    @property
    def value(self) -> str:
        """Read ValuePattern; raises ElementUnsupportedError if absent."""
        return self._native.property("value")

    @property
    def read_only(self) -> bool:
        return self._native.property("read_only")

    @property
    def toggle_state(self) -> Literal["off", "on", "indeterminate"]:
        return ("off", "on", "indeterminate")[self._native.property("toggle_state")]

    @property
    def checked(self) -> bool | None:
        """True/False for checked/unchecked; None for indeterminate."""
        state = self.toggle_state
        return None if state == "indeterminate" else state == "on"

    @property
    def patterns(self) -> tuple[str, ...]:
        return self.info().patterns

    def info(self) -> ElementInfo:
        values = self._native.info()
        values["rect"] = Rect(*values["rect"])
        values["hwnd"] = HWND(values["hwnd"]) if values["hwnd"] else None
        return ElementInfo(**values)

    def __repr__(self) -> str:
        try:
            info = self.info()
            return (
                f"Element(control_type={info.control_type!r}, name={info.name!r}, "
                f"automation_id={info.automation_id!r}, enabled={info.enabled}, "
                f"offscreen={info.offscreen}, patterns={info.patterns!r})"
            )
        except UIAutomationError:
            return "Element(unavailable=True)"

    @reporting._trace("List element children")
    def children(
        self, *, recursive: bool = False, visible_only: bool = True
    ) -> tuple[Element, ...]:
        elements = tuple(
            Element(self.window, native) for native in self._native.elements(recursive)
        )
        if visible_only:
            elements = tuple(element for element in elements if not element.offscreen)
        return elements

    def _wait(self, predicate, deadline_seconds: float, message: str) -> None:
        while not predicate():
            remaining_seconds = deadline_seconds - time.monotonic()
            if remaining_seconds <= 0:
                raise ElementTimeoutError(message)
            time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))

    def _deadline(
        self, timeout_seconds: float | None, settle_seconds: float | None
    ) -> float:
        policy = self.window.automation
        timeout = policy._resolve_timeout_seconds(timeout_seconds)
        policy._resolve_settle_seconds(settle_seconds)
        deadline = time.monotonic() + timeout
        self._wait(lambda: self.enabled, deadline, "Element did not become enabled")
        return deadline

    @reporting._trace("Invoke element")
    def invoke(
        self,
        *,
        timeout_seconds: float | None = None,
        settle_seconds: float | None = None,
    ) -> Element:
        self._deadline(timeout_seconds, settle_seconds)
        self._native.action("invoke")
        self.window.automation._settle(settle_seconds)
        return self

    @reporting._trace("Set element value")
    def set_value(
        self,
        text: str,
        *,
        timeout_seconds: float | None = None,
        settle_seconds: float | None = None,
    ) -> Element:
        if not isinstance(text, str):
            raise TypeError("Field value must be a string")
        if "\0" in text:
            raise ValueError("Field value cannot contain a null character")
        self._deadline(timeout_seconds, settle_seconds)
        self._native.action("set_value", text)
        self.window.automation._settle(settle_seconds)
        return self

    @reporting._trace("Toggle element")
    def toggle(
        self,
        *,
        timeout_seconds: float | None = None,
        settle_seconds: float | None = None,
    ) -> Element:
        self._deadline(timeout_seconds, settle_seconds)
        self._native.action("toggle")
        self.window.automation._settle(settle_seconds)
        return self

    @reporting._trace("Set checkbox state")
    def set_checked(
        self,
        checked: bool = True,
        *,
        timeout_seconds: float | None = None,
        settle_seconds: float | None = None,
    ) -> Element:
        if not isinstance(checked, bool):
            raise TypeError("Checked state must be a boolean")
        deadline = self._deadline(timeout_seconds, settle_seconds)
        # A three-state checkbox may need two transitions to reach a boolean state.
        for _ in range(3):
            before = self.toggle_state
            if (before == "on") == checked and before != "indeterminate":
                self.window.automation._settle(settle_seconds)
                return self
            self._native.action("toggle")
            self._wait(
                lambda: self.toggle_state != before,
                deadline,
                "Checkbox state did not change",
            )
        if self.checked is not checked:
            raise ElementTimeoutError("Checkbox did not reach the requested state")
        self.window.automation._settle(settle_seconds)
        return self


def elements(
    window: Window, *, recursive: bool, visible_only: bool
) -> tuple[Element, ...]:
    natives = _uia.from_handle(int(window.hwnd)).elements(recursive)
    result = tuple(Element(window, native) for native in natives)
    return (
        tuple(element for element in result if not element.offscreen)
        if visible_only
        else result
    )


def find_elements(
    window: Window,
    name: TextSelector | None,
    *,
    automation_id: TextSelector | None,
    control_type: TextSelector | None,
    class_name: TextSelector | None,
    recursive: bool,
    visible_only: bool,
) -> tuple[Element, ...]:
    if (
        isinstance(control_type, str)
        and control_type not in _uia.CONTROL_TYPES.values()
    ):
        raise ValueError(
            f"Unknown control type {control_type!r}; use a UIA type such as 'Button', 'Edit', or 'CheckBox'"
        )
    result = []
    for element in elements(window, recursive=recursive, visible_only=visible_only):
        try:
            if (
                _matches(element.name, name)
                and _matches(element.automation_id, automation_id)
                and _matches(element.control_type, control_type)
                and _matches(element.class_name, class_name)
            ):
                result.append(element)
        except ElementUnavailableError:
            # A control may disappear while a live tree is being enumerated.
            continue
    return tuple(result)


def find_element(
    window: Window,
    name: TextSelector | None,
    *,
    automation_id: TextSelector | None,
    control_type: TextSelector | None,
    class_name: TextSelector | None,
    recursive: bool,
    visible_only: bool,
    timeout_seconds: float | None,
    retry_on_ambiguity: bool | None,
) -> Element:
    timeout = window.automation._resolve_timeout_seconds(timeout_seconds)
    retry = (
        window.automation.retry_on_ambiguity
        if retry_on_ambiguity is None
        else retry_on_ambiguity
    )
    deadline = time.monotonic() + timeout
    matches = ()
    while True:
        matches = find_elements(
            window,
            name,
            automation_id=automation_id,
            control_type=control_type,
            class_name=class_name,
            recursive=recursive,
            visible_only=visible_only,
        )
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1 and not retry:
            raise ElementAmbiguousError(matches)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if len(matches) > 1:
                raise ElementAmbiguousError(matches)
            selectors = {
                "name": name,
                "automation_id": automation_id,
                "control_type": control_type,
                "class_name": class_name,
            }
            description = (
                ", ".join(
                    f"{key}={value!r}"
                    for key, value in selectors.items()
                    if value is not None
                )
                or "the supplied selectors"
            )
            raise ElementNotFoundError(
                f"No element of {int(window.hwnd)} matched {description} within {timeout:.3f}s"
            )
        time.sleep(min(_POLL_INTERVAL_SECONDS, remaining))
