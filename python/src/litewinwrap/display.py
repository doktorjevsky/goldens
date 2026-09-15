from __future__ import annotations

from dataclasses import dataclass
from math import isfinite

from . import win32
from .types import Rect


_DEFAULT_DPI = 96
_MINIMUM_SCALE = 1.0
_MAXIMUM_SCALE = 5.0


class DisplayScaleRestartRequired(RuntimeError):
    """The display scale was configured but needs a new Windows session."""

    def __init__(self, scale: float) -> None:
        self.scale = scale
        super().__init__(
            f"Display scale was configured as {scale:.0%}; "
            "sign out of Windows and rerun the script"
        )


@dataclass(frozen=True, slots=True)
class Display:
    """One active Windows display."""

    name: str
    rect: Rect
    work_area: Rect
    primary: bool
    dpi: int

    @property
    def scale(self) -> float:
        return self.dpi / _DEFAULT_DPI

    @classmethod
    def active(cls) -> tuple[Display, ...]:
        """Return all displays that belong to the active Windows desktop."""

        return tuple(
            cls(
                name=values.name,
                rect=values.rect,
                work_area=values.work_area,
                primary=values.primary,
                dpi=values.dpi,
            )
            for values in win32.enum_displays()
        )

    @classmethod
    def set_scale(cls, scale: float) -> None:
        """Set one scale for all displays, or do nothing when it already matches."""

        requested = float(scale)
        if (
            not isfinite(requested)
            or requested < _MINIMUM_SCALE
            or requested > _MAXIMUM_SCALE
        ):
            raise ValueError("Display scale must be finite and between 1.0 and 5.0")

        expected_dpi = round(_DEFAULT_DPI * requested)
        displays = cls.active()
        if not displays:
            raise OSError("Windows reported no active displays")
        if all(display.dpi == expected_dpi for display in displays):
            return

        win32.configure_global_display_scale(requested)
        if all(display.dpi == expected_dpi for display in cls.active()):
            return
        raise DisplayScaleRestartRequired(requested)
