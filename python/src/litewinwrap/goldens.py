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


class _InvalidJsonError(ValueError):
    pass


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _InvalidJsonError(f"Duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> Any:
    raise _InvalidJsonError(f"Invalid JSON number: {value}")


def _validate_json_unicode(value: Any) -> None:
    if isinstance(value, str):
        try:
            value.encode("utf-8")
        except UnicodeEncodeError as error:
            raise _InvalidJsonError("JSON contains an unpaired surrogate") from error
    elif isinstance(value, list):
        for item in value:
            _validate_json_unicode(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            _validate_json_unicode(key)
            _validate_json_unicode(item)


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
        document = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_unique_json_object,
            parse_constant=_reject_json_constant,
        )
        _validate_json_unicode(document)
    except (
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        _InvalidJsonError,
        RecursionError,
    ) as error:
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


def _read_boundary(
    annotation: dict[str, Any],
    name: str,
    image_width: int,
    image_height: int,
) -> tuple[int, int, int, int]:
    boundary = annotation.get("boundary")
    if not isinstance(boundary, dict):
        raise GoldensFormatError(f"Annotation {name!r} has no boundary")

    x = _integer(boundary, "x", name)
    y = _integer(boundary, "y", name)
    width = _integer(boundary, "width", name)
    height = _integer(boundary, "height", name)
    if x < 0 or y < 0 or width <= 0 or height <= 0:
        raise GoldensFormatError(f"Annotation {name!r} has an invalid boundary")
    if x + width > image_width or y + height > image_height:
        raise GoldensFormatError(
            f"Annotation {name!r} extends outside the golden image"
        )
    return x, y, width, height


def _read_click(annotation: dict[str, Any], name: str) -> tuple[float, float] | None:
    value = annotation.get("click")
    if value is None:
        return None
    if not isinstance(value, dict):
        raise GoldensFormatError(f"Annotation {name!r} has an invalid click point")

    x = value.get("x")
    y = value.get("y")
    if (
        not isinstance(x, (int, float))
        or isinstance(x, bool)
        or not isinstance(y, (int, float))
        or isinstance(y, bool)
        or not 0.0 <= float(x) <= 1.0
        or not 0.0 <= float(y) <= 1.0
    ):
        raise GoldensFormatError(f"Annotation {name!r} has an invalid click point")
    return float(x), float(y)


def _read_target(
    image: np.ndarray,
    value: Any,
    index: int,
    namespace: str,
) -> Target:
    if not isinstance(value, dict):
        raise GoldensFormatError(f"Annotation {index} must be an object")

    name = value.get("name")
    if not isinstance(name, str) or not name:
        raise GoldensFormatError(f"Annotation {index} has an invalid name")
    if "/" in name:
        raise GoldensFormatError(
            f"Annotation {name!r} contains '/', which is reserved for namespaces"
        )

    image_height, image_width = image.shape[:2]
    x, y, width, height = _read_boundary(value, name, image_width, image_height)
    pixels = image[y : y + height, x : x + width].copy()
    pixels.setflags(write=False)
    return Target(
        name=f"{namespace}/{name}",
        pixels=pixels,
        click=_read_click(value, name),
    )


def _resource_targets(png: Path, namespace: str) -> list[Target]:
    image = _load_image(png)
    document = _load_sidecar(png.with_suffix(".json"))
    return [
        _read_target(image, value, index, namespace)
        for index, value in enumerate(document["annotations"])
    ]


def _pngs_below(root: Path) -> list[Path]:
    try:
        candidates = root.rglob("*")
        return sorted(
            (
                path
                for path in candidates
                if path.suffix.casefold() == ".png"
                and not path.is_symlink()
                and path.is_file()
                and path.with_suffix(".json").is_file()
            ),
            key=lambda path: (
                path.relative_to(root).as_posix().casefold(),
                path.relative_to(root).as_posix(),
            ),
        )
    except OSError as error:
        raise GoldensFormatError(f"Could not scan golden root: {root}") from error


class Goldens(Mapping[str, Target]):
    """One or more golden resources exposed as a read-only target mapping."""

    def __init__(self) -> None:
        raise TypeError("Use Goldens.from_png() or Goldens.from_root()")

    @classmethod
    def _from_resources(
        cls,
        resources: list[tuple[Path, str]],
        *,
        root: Path | None,
    ) -> Goldens:
        result = cls.__new__(cls)
        targets: dict[str, Target] = {}
        identifiers: dict[str, str] = {}

        for png, namespace in resources:
            for target in _resource_targets(png, namespace):
                folded = target.name.casefold()
                previous = identifiers.get(folded)
                if previous is not None:
                    raise GoldensFormatError(
                        f"Duplicate target identifier: {previous!r} and "
                        f"{target.name!r}"
                    )
                identifiers[folded] = target.name
                targets[target.name] = target

        result._targets = targets
        result._paths = tuple(png for png, _namespace in resources)
        result._root = root
        return result

    @classmethod
    def from_png(cls, png: str | Path) -> Goldens:
        """Load one PNG/JSON pair using the PNG stem as its namespace."""

        path = Path(png)
        if path.suffix.casefold() != ".png":
            raise GoldensFormatError(f"Golden image must be a PNG: {path}")
        return cls._from_resources([(path, path.stem)], root=None)

    @classmethod
    def from_root(cls, root: str | Path) -> Goldens:
        """Recursively load every annotated PNG below a namespace root."""

        root_path = Path(root)
        if not root_path.is_dir():
            raise GoldensFormatError(
                f"Golden root is not a directory: {root_path}"
            )
        resources = [
            (
                png,
                png.relative_to(root_path).with_suffix("").as_posix(),
            )
            for png in _pngs_below(root_path)
        ]
        return cls._from_resources(resources, root=root_path)

    @property
    def paths(self) -> tuple[Path, ...]:
        return self._paths

    @property
    def root(self) -> Path | None:
        return self._root

    def __getitem__(self, name: str) -> Target:
        return self._targets[name]

    def __iter__(self) -> Iterator[str]:
        return iter(self._targets)

    def __len__(self) -> int:
        return len(self._targets)
