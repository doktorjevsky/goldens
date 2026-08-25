from __future__ import annotations

from dataclasses import dataclass
from typing import NewType, NamedTuple

import numpy as np
from numpy.typing import NDArray


HWND = NewType("HWND", int)
Pixels = NDArray[np.uint8]


class Point(NamedTuple):
    x: int
    y: int


class Rect(NamedTuple):
    left: int
    top: int
    right: int
    bottom: int

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top


@dataclass(frozen=True, slots=True)
class Capture:
    """One screen capture and the absolute rectangle its pixels came from."""

    pixels: Pixels
    rect: Rect


@dataclass(frozen=True, slots=True)
class Target:
    """A visual template, optionally with a normalized click point."""

    name: str
    pixels: Pixels
    click: tuple[float, float] | None = None


@dataclass(frozen=True, slots=True)
class Match:
    """The result of matching a target in a particular screen capture."""

    target: str
    score: float
    rect: Rect
    click: Point
