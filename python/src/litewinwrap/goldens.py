from __future__ import annotations

import json
from collections.abc import Iterator, Mapping
from pathlib import Path
from typing import Any

import cv2
import numpy as np

from .types import Target


class GoldensFormatError(ValueError):
    pass


def _load_image(path: Path) -> np.ndarray:
    try:
        encoded = np.fromfile(path, dtype=np.uint8)
    except OSError as error:
        raise GoldensFormatError(f"Could not read golden image: {path}") from error
    pixels = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if pixels is None:
        raise GoldensFormatError(f"Could not decode golden image: {path}")
    return pixels


def _load_sidecar(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise GoldensFormatError(f"Could not read golden sidecar: {path}") from error
    if not isinstance(document, dict) or not isinstance(
        document.get("annotations"), list
    ):
        raise GoldensFormatError(f"Invalid golden sidecar: {path}")
    return document


def _integer(value: dict[str, Any], key: str, name: str) -> int:
    result = value.get(key)
    if not isinstance(result, int) or isinstance(result, bool):
        raise GoldensFormatError(
            f"Annotation {name!r} boundary {key!r} must be an integer"
        )
    return result


class Goldens(Mapping[str, Target]):
    """A Goldens PNG/JSON pair exposed as a read-only mapping of targets."""

    def __init__(self, png: str | Path):
        self._path = Path(png)
        image = _load_image(self._path)
        document = _load_sidecar(self._path.with_suffix(".json"))
        image_height, image_width = image.shape[:2]
        targets: dict[str, Target] = {}

        for index, value in enumerate(document["annotations"]):
            if not isinstance(value, dict):
                raise GoldensFormatError(f"Annotation {index} must be an object")
            name = value.get("name")
            if not isinstance(name, str) or not name:
                raise GoldensFormatError(f"Annotation {index} has an invalid name")
            if name in targets:
                raise GoldensFormatError(f"Duplicate annotation name: {name!r}")

            boundary = value.get("boundary")
            if not isinstance(boundary, dict):
                raise GoldensFormatError(f"Annotation {name!r} has no boundary")
            x = _integer(boundary, "x", name)
            y = _integer(boundary, "y", name)
            width = _integer(boundary, "width", name)
            height = _integer(boundary, "height", name)
            if x < 0 or y < 0 or width <= 0 or height <= 0:
                raise GoldensFormatError(
                    f"Annotation {name!r} has an invalid boundary"
                )
            if x + width > image_width or y + height > image_height:
                raise GoldensFormatError(
                    f"Annotation {name!r} extends outside the golden image"
                )

            click_value = value.get("click")
            click: tuple[float, float] | None = None
            if click_value is not None:
                if not isinstance(click_value, dict):
                    raise GoldensFormatError(
                        f"Annotation {name!r} has an invalid click point"
                    )
                click_x = click_value.get("x")
                click_y = click_value.get("y")
                if (
                    not isinstance(click_x, (int, float))
                    or isinstance(click_x, bool)
                    or not isinstance(click_y, (int, float))
                    or isinstance(click_y, bool)
                    or not 0.0 <= float(click_x) <= 1.0
                    or not 0.0 <= float(click_y) <= 1.0
                ):
                    raise GoldensFormatError(
                        f"Annotation {name!r} has an invalid click point"
                    )
                click = (float(click_x), float(click_y))

            pixels = image[y : y + height, x : x + width].copy()
            pixels.setflags(write=False)
            targets[name] = Target(name=name, pixels=pixels, click=click)

        self._targets = targets

    @property
    def path(self) -> Path:
        return self._path

    def __getitem__(self, name: str) -> Target:
        return self._targets[name]

    def __iter__(self) -> Iterator[str]:
        return iter(self._targets)

    def __len__(self) -> int:
        return len(self._targets)
