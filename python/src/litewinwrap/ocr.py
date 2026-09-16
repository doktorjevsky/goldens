from __future__ import annotations

import os
import threading
from functools import cache
from math import ceil, isfinite
from pathlib import Path
from typing import TypeAlias

import cv2
import numpy as np

from .automation import TextSelector, _matches
from .types import Capture, Pixels, Point, Rect, TextMatch


# Detection post-processing follows the DB pipeline used by PaddleOCR and
# RapidOCR. Those projects and the bundled converted models are Apache-2.0;
# attribution and exact artifact hashes are in models/NOTICE.md.
_MODEL_DIRECTORY = Path(__file__).with_name("models")
_DETECTION_MODEL = _MODEL_DIRECTORY / "en_PP-OCRv3_det_mobile.onnx"
_RECOGNITION_MODEL = _MODEL_DIRECTORY / "latin_PP-OCRv3_rec_mobile.onnx"
_CHARACTER_DICTIONARY = _MODEL_DIRECTORY / "latin_dict.txt"

_DETECTION_MIN_SIDE = 736
_DETECTION_MAX_SIDE = 1600
_DETECTION_THRESHOLD = 0.3
_DETECTION_BOX_THRESHOLD = 0.5
_DETECTION_UNCLIP_RATIO = 1.6
_DETECTION_MAX_CANDIDATES = 1000
_RECOGNITION_HEIGHT = 48
_RECOGNITION_WIDTH = 320
# OpenCV's ONNX backend does not propagate dynamic batch dimensions through
# every layer in this model, so recognition is intentionally one crop at a time.
_RECOGNITION_BATCH_SIZE = 1
_READING_LINE_TOLERANCE = 10

ImageSource: TypeAlias = Capture | Pixels | str | os.PathLike[str]


class OcrImageError(ValueError):
    pass


class _Models:
    def __init__(self) -> None:
        self.detector = cv2.dnn.readNetFromONNX(str(_DETECTION_MODEL))
        self.recognizer = cv2.dnn.readNetFromONNX(str(_RECOGNITION_MODEL))
        dictionary = _CHARACTER_DICTIONARY.read_text(encoding="utf-8").splitlines()
        self.characters = ("blank", *dictionary, " ")


_INFERENCE_LOCK = threading.Lock()


@cache
def _models() -> _Models:
    return _Models()


def _score(value: float) -> float:
    result = float(value)
    if not isfinite(result) or not 0.0 <= result <= 1.0:
        raise ValueError("OCR minimum score must be between 0 and 1")
    return result


def _read_path(path: str | os.PathLike[str]) -> Pixels:
    source = Path(path)
    try:
        encoded = np.fromfile(source, dtype=np.uint8)
    except OSError as error:
        raise OcrImageError(f"Could not read image: {source}") from error
    pixels = cv2.imdecode(encoded, cv2.IMREAD_UNCHANGED)
    if pixels is None:
        raise OcrImageError(f"Could not decode image: {source}")
    return pixels


def _image(source: ImageSource) -> tuple[Pixels, Point]:
    if isinstance(source, Capture):
        pixels = source.pixels
        height, width = pixels.shape[:2]
        if source.rect.width != width or source.rect.height != height:
            raise OcrImageError("Capture rectangle does not match its pixel dimensions")
        origin = Point(source.rect.left, source.rect.top)
    elif isinstance(source, (str, os.PathLike)):
        pixels = _read_path(source)
        origin = Point(0, 0)
    elif isinstance(source, np.ndarray):
        pixels = source
        origin = Point(0, 0)
    else:
        raise TypeError("OCR image must be a path, pixel array, or Capture")

    if pixels.dtype != np.uint8:
        raise OcrImageError("OCR pixels must use the uint8 data type")
    if pixels.ndim == 2:
        pixels = cv2.cvtColor(pixels, cv2.COLOR_GRAY2BGR)
    elif pixels.ndim == 3 and pixels.shape[2] == 4:
        pixels = cv2.cvtColor(pixels, cv2.COLOR_BGRA2BGR)
    elif pixels.ndim != 3 or pixels.shape[2] != 3:
        raise OcrImageError("OCR pixels must be grayscale, BGR, or BGRA")
    if pixels.shape[0] == 0 or pixels.shape[1] == 0:
        raise OcrImageError("OCR image is empty")
    return np.ascontiguousarray(pixels), origin


def _aligned_detection_size(height: int, width: int) -> tuple[int, int]:
    scale = max(1.0, _DETECTION_MIN_SIDE / min(height, width))
    if max(height, width) * scale > _DETECTION_MAX_SIDE:
        scale = _DETECTION_MAX_SIDE / max(height, width)
    resized_height = max(32, int(round(height * scale / 32.0) * 32))
    resized_width = max(32, int(round(width * scale / 32.0) * 32))
    return resized_height, resized_width


def _mini_box(contour: np.ndarray) -> tuple[np.ndarray, float]:
    rotated = cv2.minAreaRect(contour.astype(np.float32))
    points = sorted(cv2.boxPoints(rotated).tolist(), key=lambda point: point[0])
    left = sorted(points[:2], key=lambda point: point[1])
    right = sorted(points[2:], key=lambda point: point[1])
    box = np.array((left[0], right[0], right[1], left[1]), dtype=np.float32)
    return box, min(rotated[1])


def _box_score(bitmap: np.ndarray, points: np.ndarray) -> float:
    height, width = bitmap.shape
    left = int(np.clip(np.floor(points[:, 0].min()), 0, width - 1))
    right = int(np.clip(np.ceil(points[:, 0].max()), 0, width - 1))
    top = int(np.clip(np.floor(points[:, 1].min()), 0, height - 1))
    bottom = int(np.clip(np.ceil(points[:, 1].max()), 0, height - 1))
    mask = np.zeros((bottom - top + 1, right - left + 1), dtype=np.uint8)
    local = points.copy()
    local[:, 0] -= left
    local[:, 1] -= top
    cv2.fillPoly(mask, local.astype(np.int32).reshape(1, -1, 2), 1)
    return float(cv2.mean(bitmap[top : bottom + 1, left : right + 1], mask)[0])


def _unclip(box: np.ndarray) -> np.ndarray:
    rotated = cv2.minAreaRect(box.astype(np.float32))
    width, height = rotated[1]
    perimeter = 2.0 * (width + height)
    if perimeter == 0.0:
        return box
    distance = width * height * _DETECTION_UNCLIP_RATIO / perimeter
    expanded = (
        rotated[0],
        (width + 2.0 * distance, height + 2.0 * distance),
        rotated[2],
    )
    return cv2.boxPoints(expanded)


def _sort_boxes(
    boxes: list[tuple[np.ndarray, float]],
) -> list[tuple[np.ndarray, float]]:
    boxes.sort(key=lambda item: float(item[0][:, 1].min()))
    lines: list[list[tuple[np.ndarray, float]]] = []
    line_tops: list[float] = []
    for item in boxes:
        top = float(item[0][:, 1].min())
        if not lines or abs(top - line_tops[-1]) >= _READING_LINE_TOLERANCE:
            lines.append([item])
            line_tops.append(top)
            continue
        lines[-1].append(item)
    for line in lines:
        line.sort(key=lambda item: float(item[0][:, 0].min()))
    return [item for line in lines for item in line]


def _detect(pixels: Pixels, detector: cv2.dnn.Net) -> list[tuple[np.ndarray, float]]:
    source_height, source_width = pixels.shape[:2]
    resized_height, resized_width = _aligned_detection_size(source_height, source_width)
    interpolation = (
        cv2.INTER_AREA
        if resized_height < source_height or resized_width < source_width
        else cv2.INTER_LINEAR
    )
    resized = cv2.resize(
        pixels,
        (resized_width, resized_height),
        interpolation=interpolation,
    )
    blob = resized.astype(np.float32) / 127.5 - 1.0
    detector.setInput(blob.transpose(2, 0, 1)[np.newaxis, :])
    prediction = detector.forward()[0, 0]
    bitmap = cv2.dilate(
        (prediction > _DETECTION_THRESHOLD).astype(np.uint8),
        np.ones((2, 2), dtype=np.uint8),
    )
    contours, _hierarchy = cv2.findContours(
        bitmap * 255,
        cv2.RETR_LIST,
        cv2.CHAIN_APPROX_SIMPLE,
    )

    boxes: list[tuple[np.ndarray, float]] = []
    scale_x = source_width / prediction.shape[1]
    scale_y = source_height / prediction.shape[0]
    for contour in contours[:_DETECTION_MAX_CANDIDATES]:
        box, short_side = _mini_box(contour)
        if short_side < 3.0:
            continue
        detection_score = _box_score(prediction, box)
        if detection_score < _DETECTION_BOX_THRESHOLD:
            continue
        box, short_side = _mini_box(_unclip(box))
        if short_side < 5.0:
            continue
        box[:, 0] = np.clip(np.round(box[:, 0] * scale_x), 0, source_width - 1)
        box[:, 1] = np.clip(np.round(box[:, 1] * scale_y), 0, source_height - 1)
        box, _short_side = _mini_box(box)
        box[:, 0] = np.clip(box[:, 0], 0, source_width - 1)
        box[:, 1] = np.clip(box[:, 1], 0, source_height - 1)
        box_width = np.linalg.norm(box[0] - box[1])
        box_height = np.linalg.norm(box[0] - box[3])
        if box_width <= 3.0 or box_height <= 3.0:
            continue
        boxes.append((box, detection_score))
    return _sort_boxes(boxes)


def _crop(pixels: Pixels, box: np.ndarray) -> Pixels:
    width = max(
        1,
        int(
            max(
                np.linalg.norm(box[0] - box[1]),
                np.linalg.norm(box[2] - box[3]),
            )
        ),
    )
    height = max(
        1,
        int(
            max(
                np.linalg.norm(box[0] - box[3]),
                np.linalg.norm(box[1] - box[2]),
            )
        ),
    )
    destination = np.array(
        ((0, 0), (width, 0), (width, height), (0, height)),
        dtype=np.float32,
    )
    transform = cv2.getPerspectiveTransform(box.astype(np.float32), destination)
    result = cv2.warpPerspective(
        pixels,
        transform,
        (width, height),
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_REPLICATE,
    )
    if result.shape[0] / result.shape[1] >= 1.5:
        result = np.rot90(result)
    return result


def _recognition_input(image: Pixels, width: int) -> np.ndarray:
    height, source_width = image.shape[:2]
    resized_width = min(width, int(ceil(_RECOGNITION_HEIGHT * source_width / height)))
    resized = cv2.resize(image, (max(1, resized_width), _RECOGNITION_HEIGHT))
    normalized = resized.astype(np.float32).transpose(2, 0, 1) / 127.5 - 1.0
    padded = np.zeros((3, _RECOGNITION_HEIGHT, width), dtype=np.float32)
    padded[:, :, :resized_width] = normalized
    return padded


def _decode(prediction: np.ndarray, characters: tuple[str, ...]) -> tuple[str, float]:
    if prediction.shape[1] != len(characters):
        raise RuntimeError(
            "OCR recognition model and character dictionary are incompatible"
        )
    indices = prediction.argmax(axis=1)
    probabilities = prediction.max(axis=1)
    selected = indices != 0
    selected[1:] &= indices[1:] != indices[:-1]
    selected_indices = indices[selected]
    text = "".join(characters[int(index)] for index in selected_indices)
    if not len(selected_indices):
        return "", 0.0
    return text, float(probabilities[selected].mean())


def _recognize(
    crops: list[Pixels],
    recognizer: cv2.dnn.Net,
    characters: tuple[str, ...],
) -> list[tuple[str, float]]:
    ratios = [image.shape[1] / image.shape[0] for image in crops]
    order = sorted(range(len(crops)), key=ratios.__getitem__)
    results: list[tuple[str, float]] = [("", 0.0)] * len(crops)
    for start in range(0, len(order), _RECOGNITION_BATCH_SIZE):
        indices = order[start : start + _RECOGNITION_BATCH_SIZE]
        ratio = max(
            _RECOGNITION_WIDTH / _RECOGNITION_HEIGHT, *(ratios[i] for i in indices)
        )
        width = max(_RECOGNITION_WIDTH, int(ceil(_RECOGNITION_HEIGHT * ratio)))
        batch = np.stack([_recognition_input(crops[index], width) for index in indices])
        recognizer.setInput(batch)
        predictions = recognizer.forward()
        for index, prediction in zip(indices, predictions):
            results[index] = _decode(prediction, characters)
    return results


def read(
    image: ImageSource,
    *,
    min_score: float = 0.5,
) -> tuple[TextMatch, ...]:
    """Recognize text regions in a path, BGR pixel array, or screen capture."""

    threshold = _score(min_score)
    pixels, origin = _image(image)
    with _INFERENCE_LOCK:
        models = _models()
        boxes = _detect(pixels, models.detector)
        if not boxes:
            return ()
        crops = [_crop(pixels, box) for box, _score_value in boxes]
        recognized = _recognize(crops, models.recognizer, models.characters)

    matches: list[TextMatch] = []
    for (box, _detection_score), (text, recognition_score) in zip(boxes, recognized):
        if not text or recognition_score < threshold:
            continue
        left = origin.x + int(np.floor(box[:, 0].min()))
        top = origin.y + int(np.floor(box[:, 1].min()))
        right = origin.x + int(np.ceil(box[:, 0].max())) + 1
        bottom = origin.y + int(np.ceil(box[:, 1].max())) + 1
        rect = Rect(left, top, right, bottom)
        click = Point(
            min(rect.right - 1, rect.left + rect.width // 2),
            min(rect.bottom - 1, rect.top + rect.height // 2),
        )
        matches.append(TextMatch(text, recognition_score, rect, click))
    return tuple(matches)


def find(
    image: ImageSource,
    selector: TextSelector,
    *,
    min_score: float = 0.5,
) -> tuple[TextMatch, ...]:
    """Return recognized regions whose text exactly or regex-matches a selector."""

    return tuple(
        item
        for item in read(image, min_score=min_score)
        if _matches(item.text, selector)
    )
