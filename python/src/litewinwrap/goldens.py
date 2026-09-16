from __future__ import annotations

import json
from collections.abc import Iterator, Mapping
from math import isfinite
from pathlib import Path
from typing import Any

import cv2
import numpy as np

from .types import Target


class GoldensFormatError(ValueError):
    pass


class InconsistentGoldenScaleError(GoldensFormatError):
    """Raised when resources in one golden tree use different scales."""

    def __init__(
        self,
        expected_path: Path,
        expected_scale: float,
        conflicting_path: Path,
        conflicting_scale: float,
    ) -> None:
        self.expected_path = expected_path
        self.expected_scale = expected_scale
        self.conflicting_path = conflicting_path
        self.conflicting_scale = conflicting_scale
        super().__init__(
            "Golden tree contains inconsistent capture scales: "
            f"{expected_path} uses {expected_scale:g}, but "
            f"{conflicting_path} uses {conflicting_scale:g}"
        )


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


def _read_scale(document: dict[str, Any], path: Path) -> float | None:
    if "scale" not in document:
        return None
    value = document["scale"]
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or value <= 0
    ):
        raise GoldensFormatError(
            f"Golden sidecar scale must be a finite positive number: {path}"
        )
    try:
        scale = float(value)
    except OverflowError as error:
        raise GoldensFormatError(
            f"Golden sidecar scale must be a finite positive number: {path}"
        ) from error
    if not isfinite(scale):
        raise GoldensFormatError(
            f"Golden sidecar scale must be a finite positive number: {path}"
        )
    return scale


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
    if "\\" in name:
        raise GoldensFormatError(
            f"Annotation {name!r} contains '\\', which is reserved for namespaces"
        )

    image_height, image_width = image.shape[:2]
    x, y, width, height = _read_boundary(value, name, image_width, image_height)
    pixels = image[y : y + height, x : x + width].copy()
    pixels.setflags(write=False)
    return Target(
        name=f"{namespace}/{name}" if namespace else name,
        pixels=pixels,
        click=_read_click(value, name),
    )


def _resource_targets(
    png: Path,
    namespace: str,
) -> tuple[list[Target], float | None]:
    sidecar = png.with_suffix(".json")
    document = _load_sidecar(sidecar)
    scale = _read_scale(document, sidecar)
    image = _load_image(png)
    targets = [
        _read_target(image, value, index, namespace)
        for index, value in enumerate(document["annotations"])
    ]
    return targets, scale


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
        common_scale: float | None = None
        common_scale_path: Path | None = None

        for png, namespace in resources:
            resource_targets, resource_scale = _resource_targets(png, namespace)
            if root is not None and resource_scale is None:
                raise GoldensFormatError(
                    f"Golden sidecar has no capture scale: {png.with_suffix('.json')}"
                )
            if resource_scale is not None:
                if common_scale is None:
                    common_scale = resource_scale
                    common_scale_path = png
                elif resource_scale != common_scale:
                    assert common_scale_path is not None
                    raise InconsistentGoldenScaleError(
                        common_scale_path,
                        common_scale,
                        png,
                        resource_scale,
                    )

            for target in resource_targets:
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
        result._scale = common_scale
        return result

    @classmethod
    def from_png(cls, png: str | Path) -> Goldens:
        """Load one PNG/JSON pair with bare annotation identifiers."""

        path = Path(png)
        if path.suffix.casefold() != ".png":
            raise GoldensFormatError(f"Golden image must be a PNG: {path}")
        return cls._from_resources([(path, "")], root=None)

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
                ""
                if png.parent == root_path
                else png.parent.relative_to(root_path).as_posix(),
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

    @property
    def scale(self) -> float | None:
        """Return the shared capture scale, or None for legacy single resources."""

        return self._scale

    def __getitem__(self, name: str) -> Target:
        return self._targets[name]

    def __iter__(self) -> Iterator[str]:
        return iter(self._targets)

    def __len__(self) -> int:
        return len(self._targets)
