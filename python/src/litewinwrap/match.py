from __future__ import annotations

import time
from math import isfinite

import cv2
import numpy as np

from . import mouse, reporting, win32
from .types import Capture, HWND, Match, Point, Rect, Target
from .window import Window


_POLL_INTERVAL_SECONDS = 0.05
_POST_CLICK_DELAY_SECONDS = 0.15
_MAX_TARGET_SCALES = 101


class TargetNotFoundError(LookupError):
    def __init__(
        self,
        target: Target,
        *,
        threshold: float,
        best_score: float,
        elapsed_seconds: float,
        attempts: int,
        last_capture: Capture | None,
    ) -> None:
        self.target = target
        self.threshold = threshold
        self.best_score = best_score
        self.elapsed_seconds = elapsed_seconds
        self.attempts = attempts
        self.last_capture = last_capture
        super().__init__(
            f"Target {target.name!r} was not found within {elapsed_seconds:.3f}s; "
            f"best score {best_score:.4f}, threshold {threshold:.4f}"
        )


class TargetAmbiguousError(LookupError):
    def __init__(self, target: Target, matches: tuple[Match, ...]) -> None:
        self.target = target
        self.matches = matches
        super().__init__(
            f"Target {target.name!r} matched {len(matches)} distinct locations"
        )


def _hwnd(window: Window | HWND | int) -> HWND:
    return window.hwnd if isinstance(window, Window) else HWND(int(window))


def capture(window: Window | HWND | int) -> Capture:
    with reporting._capture_guard():
        hwnd = _hwnd(window)
        if not win32.is_window(hwnd):
            raise LookupError(f"Window no longer exists: {int(hwnd)}")
        rect = win32.get_extended_frame_rect(hwnd)
        raw = win32.capture_screen(rect)
        bgra = np.frombuffer(raw, dtype=np.uint8).reshape(rect.height, rect.width, 4)
        pixels = bgra[:, :, :3].copy()
        pixels.setflags(write=False)
        return Capture(pixels=pixels, rect=rect)


def _validate(capture_value: Capture, target: Target, threshold: float) -> None:
    if not 0.0 <= threshold <= 1.0:
        raise ValueError("Match threshold must be between 0 and 1")
    if capture_value.pixels.ndim not in (2, 3):
        raise ValueError("Capture pixels must be a grayscale or colour image")
    if target.pixels.ndim != capture_value.pixels.ndim:
        raise ValueError("Capture and target must have the same channel layout")
    if capture_value.pixels.ndim == 3:
        if target.pixels.shape[2] != capture_value.pixels.shape[2]:
            raise ValueError("Capture and target must have the same channel count")
    image_height, image_width = capture_value.pixels.shape[:2]
    target_height, target_width = target.pixels.shape[:2]
    if (
        capture_value.rect.width != image_width
        or capture_value.rect.height != image_height
    ):
        raise ValueError("Capture rectangle does not match its pixel dimensions")
    if target_width <= 0 or target_height <= 0:
        raise ValueError("Target image is empty")
    if target.click is not None and not all(
        0.0 <= value <= 1.0 for value in target.click
    ):
        raise ValueError("Target click coordinates must be between 0 and 1")


def _scale_values(tolerance: float, step: float) -> tuple[float, ...]:
    tolerance = float(tolerance)
    step = float(step)
    if not isfinite(tolerance) or not 0.0 <= tolerance < 1.0:
        raise ValueError(
            "Target scale tolerance must be finite, at least 0, and less than 1"
        )
    if not isfinite(step) or step <= 0.0:
        raise ValueError("Target scale step must be a finite positive number")
    if tolerance == 0.0:
        return (1.0,)
    scale_count = 2 * int(np.ceil(tolerance / step)) + 1
    if scale_count > _MAX_TARGET_SCALES:
        raise ValueError(
            f"Target scale search may evaluate at most {_MAX_TARGET_SCALES} scales"
        )

    offsets: list[float] = []
    offset = step
    while offset < tolerance:
        offsets.append(offset)
        offset += step
    if not offsets or not np.isclose(offsets[-1], tolerance):
        offsets.append(tolerance)

    values = [1.0]
    for offset in offsets:
        values.extend((1.0 - offset, 1.0 + offset))
    return tuple(values)


def _scaled_targets(
    target: Target,
    tolerance: float,
    step: float,
) -> tuple[tuple[Target, float], ...]:
    height, width = target.pixels.shape[:2]
    variants: list[tuple[Target, float]] = []
    dimensions: set[tuple[int, int]] = set()
    for scale in _scale_values(tolerance, step):
        scaled_width = max(1, round(width * scale))
        scaled_height = max(1, round(height * scale))
        size = (scaled_width, scaled_height)
        if size in dimensions:
            continue
        dimensions.add(size)
        if size == (width, height):
            variants.append((target, scale))
            continue

        interpolation = cv2.INTER_AREA if scale < 1.0 else cv2.INTER_LINEAR
        pixels = cv2.resize(target.pixels, size, interpolation=interpolation)
        pixels.setflags(write=False)
        variants.append((Target(target.name, pixels, target.click), scale))
    return tuple(variants)


def _score_map(capture_value: Capture, target: Target) -> np.ndarray:
    # CCOEFF is discriminating for normal UI crops. SQDIFF handles flat-colour
    # targets for which CCOEFF's variance term is undefined.
    spatial_deviation = np.std(
        target.pixels.astype(np.float32),
        axis=(0, 1),
    )
    if float(np.max(spatial_deviation)) < 1.0:
        capture_pixels = capture_value.pixels
        target_pixels = target.pixels
        if not np.any(target_pixels):
            # SQDIFF_NORMED has a zero denominator for an all-zero template.
            # Compare the inverse images so black has the same stable scoring
            # behaviour as an all-white flat target.
            capture_pixels = 255 - capture_pixels
            target_pixels = 255 - target_pixels
        difference = cv2.matchTemplate(
            capture_pixels,
            target_pixels,
            cv2.TM_SQDIFF_NORMED,
        )
        scores = 1.0 - difference
    else:
        scores = cv2.matchTemplate(
            capture_value.pixels,
            target.pixels,
            cv2.TM_CCOEFF_NORMED,
        )
    return np.nan_to_num(scores, nan=-1.0, posinf=1.0, neginf=-1.0)


def _intersection_over_union(left: Rect, right: Rect) -> float:
    intersection_width = max(
        0, min(left.right, right.right) - max(left.left, right.left)
    )
    intersection_height = max(
        0, min(left.bottom, right.bottom) - max(left.top, right.top)
    )
    intersection = intersection_width * intersection_height
    if not intersection:
        return 0.0
    left_area = left.width * left.height
    right_area = right.width * right.height
    return intersection / (left_area + right_area - intersection)


def _matches_and_best_score(
    capture_value: Capture,
    target: Target,
    *,
    threshold: float,
    overlap: float,
    max_candidates: int,
    target_scale_tolerance: float,
    target_scale_step: float,
) -> tuple[tuple[Match, ...], float]:
    _validate(capture_value, target, threshold)
    if not 0.0 <= overlap < 1.0:
        raise ValueError("Overlap threshold must be at least 0 and less than 1")
    if max_candidates <= 0:
        raise ValueError("Maximum candidate count must be positive")

    variants = _scaled_targets(
        target,
        target_scale_tolerance,
        target_scale_step,
    )
    image_height, image_width = capture_value.pixels.shape[:2]
    click_x, click_y = target.click or (0.5, 0.5)
    candidates: list[Match] = []
    best_score = -1.0
    fitting_variants = 0

    for scaled_target, scale in variants:
        target_height, target_width = scaled_target.pixels.shape[:2]
        if target_width > image_width or target_height > image_height:
            continue
        fitting_variants += 1
        scores = _score_map(capture_value, scaled_target)
        best_score = max(best_score, float(scores.max()))
        ys, xs = np.nonzero(scores >= threshold)
        if not len(xs):
            continue

        candidate_scores = scores[ys, xs]
        if len(xs) > max_candidates:
            selected = np.argpartition(candidate_scores, -max_candidates)[
                -max_candidates:
            ]
            xs, ys = xs[selected], ys[selected]
            candidate_scores = candidate_scores[selected]

        for index in np.argsort(candidate_scores)[::-1]:
            left = capture_value.rect.left + int(xs[index])
            top = capture_value.rect.top + int(ys[index])
            rect = Rect(left, top, left + target_width, top + target_height)
            point = Point(
                min(
                    rect.right - 1,
                    max(rect.left, rect.left + int(target_width * click_x)),
                ),
                min(
                    rect.bottom - 1,
                    max(rect.top, rect.top + int(target_height * click_y)),
                ),
            )
            candidates.append(
                Match(
                    target=target.name,
                    score=float(candidate_scores[index]),
                    rect=rect,
                    click=point,
                    scale=scale,
                )
            )

    if not fitting_variants:
        smallest, _scale = min(
            variants,
            key=lambda item: item[0].pixels.shape[0] * item[0].pixels.shape[1],
        )
        target_height, target_width = smallest.pixels.shape[:2]
        raise ValueError(
            f"Target {target_width}x{target_height} is larger than "
            f"capture {image_width}x{image_height}"
        )
    if not candidates:
        return (), best_score

    candidates.sort(key=lambda item: item.score, reverse=True)
    candidates = candidates[:max_candidates]
    matches: list[Match] = []
    for candidate in candidates:
        if any(
            _intersection_over_union(candidate.rect, item.rect) > overlap
            for item in matches
        ):
            continue
        matches.append(candidate)
    return tuple(matches), best_score


@reporting._trace("Match all targets")
def match_all(
    capture_value: Capture,
    target: Target,
    *,
    threshold: float = 0.90,
    overlap: float = 0.30,
    max_candidates: int = 10_000,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> tuple[Match, ...]:
    """Return every distinct target match in an existing capture."""

    matches, _best_score = _matches_and_best_score(
        capture_value,
        target,
        threshold=threshold,
        overlap=overlap,
        max_candidates=max_candidates,
        target_scale_tolerance=target_scale_tolerance,
        target_scale_step=target_scale_step,
    )
    return matches


def _not_found(
    capture_value: Capture,
    target: Target,
    threshold: float,
    best_score: float,
) -> TargetNotFoundError:
    return TargetNotFoundError(
        target,
        threshold=threshold,
        best_score=best_score,
        elapsed_seconds=0.0,
        attempts=1,
        last_capture=capture_value,
    )


@reporting._trace("Match target")
def match(
    capture_value: Capture,
    target: Target,
    *,
    threshold: float = 0.90,
    overlap: float = 0.30,
    max_candidates: int = 10_000,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> Match:
    """Return the only target match, rejecting zero or multiple matches."""

    matches, best_score = _matches_and_best_score(
        capture_value,
        target,
        threshold=threshold,
        overlap=overlap,
        max_candidates=max_candidates,
        target_scale_tolerance=target_scale_tolerance,
        target_scale_step=target_scale_step,
    )
    if len(matches) == 1:
        return matches[0]
    if matches:
        raise TargetAmbiguousError(target, matches)
    raise _not_found(capture_value, target, threshold, best_score)


@reporting._trace("Match best target")
def best_match(
    capture_value: Capture,
    target: Target,
    *,
    threshold: float = 0.90,
    overlap: float = 0.30,
    max_candidates: int = 10_000,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> Match:
    """Return the highest-scoring target match in an existing capture."""

    matches, best_score = _matches_and_best_score(
        capture_value,
        target,
        threshold=threshold,
        overlap=overlap,
        max_candidates=max_candidates,
        target_scale_tolerance=target_scale_tolerance,
        target_scale_step=target_scale_step,
    )
    if matches:
        return matches[0]
    raise _not_found(capture_value, target, threshold, best_score)


@reporting._trace("Find all targets", hwnd_parameter="window")
def find_all(
    window: Window | HWND | int,
    target: Target,
    *,
    threshold: float = 0.90,
    timeout_seconds: float = 0.0,
    overlap: float = 0.30,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> tuple[Match, ...]:
    deadline_seconds = time.monotonic() + max(0.0, timeout_seconds)
    while True:
        current = capture(window)
        matches, _best_score = _matches_and_best_score(
            current,
            target,
            threshold=threshold,
            overlap=overlap,
            max_candidates=10_000,
            target_scale_tolerance=target_scale_tolerance,
            target_scale_step=target_scale_step,
        )
        if matches:
            return matches

        remaining_seconds = deadline_seconds - time.monotonic()
        if remaining_seconds <= 0:
            return ()
        time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))


@reporting._trace("Find target", hwnd_parameter="window")
def find(
    window: Window | HWND | int,
    target: Target,
    *,
    threshold: float = 0.90,
    timeout_seconds: float = 0.0,
    overlap: float = 0.30,
    retry_on_ambiguity: bool = False,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> Match:
    """Wait for one target match, optionally retrying transient ambiguity."""

    started_seconds = time.monotonic()
    deadline_seconds = started_seconds + max(0.0, timeout_seconds)
    best_score = -1.0
    attempts = 0
    last_capture: Capture | None = None

    while True:
        last_capture = capture(window)
        attempts += 1
        matches, current_best = _matches_and_best_score(
            last_capture,
            target,
            threshold=threshold,
            overlap=overlap,
            max_candidates=10_000,
            target_scale_tolerance=target_scale_tolerance,
            target_scale_step=target_scale_step,
        )
        best_score = max(best_score, current_best)
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1 and not retry_on_ambiguity:
            raise TargetAmbiguousError(target, matches)

        remaining_seconds = deadline_seconds - time.monotonic()
        if remaining_seconds <= 0:
            if matches:
                raise TargetAmbiguousError(target, matches)
            elapsed_seconds = time.monotonic() - started_seconds
            raise TargetNotFoundError(
                target,
                threshold=threshold,
                best_score=best_score,
                elapsed_seconds=elapsed_seconds,
                attempts=attempts,
                last_capture=last_capture,
            )
        time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))


@reporting._trace("Find best target", hwnd_parameter="window")
def find_best(
    window: Window | HWND | int,
    target: Target,
    *,
    threshold: float = 0.90,
    timeout_seconds: float = 0.0,
    overlap: float = 0.30,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
) -> Match:
    started_seconds = time.monotonic()
    deadline_seconds = started_seconds + max(0.0, timeout_seconds)
    best_score = -1.0
    attempts = 0
    last_capture: Capture | None = None

    while True:
        last_capture = capture(window)
        attempts += 1
        matches, current_best = _matches_and_best_score(
            last_capture,
            target,
            threshold=threshold,
            overlap=overlap,
            max_candidates=10_000,
            target_scale_tolerance=target_scale_tolerance,
            target_scale_step=target_scale_step,
        )
        best_score = max(best_score, current_best)
        if matches:
            return matches[0]

        remaining_seconds = deadline_seconds - time.monotonic()
        if remaining_seconds <= 0:
            elapsed_seconds = time.monotonic() - started_seconds
            raise TargetNotFoundError(
                target,
                threshold=threshold,
                best_score=best_score,
                elapsed_seconds=elapsed_seconds,
                attempts=attempts,
                last_capture=last_capture,
            )
        time.sleep(min(_POLL_INTERVAL_SECONDS, remaining_seconds))


@reporting._trace("Find and click target", hwnd_parameter="window")
def click(
    window: Window | HWND | int,
    target: Target,
    *,
    threshold: float = 0.90,
    timeout_seconds: float = 0.0,
    overlap: float = 0.30,
    retry_on_ambiguity: bool = False,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
    button: mouse.Button = "left",
    wait_after_seconds: float = _POST_CLICK_DELAY_SECONDS,
) -> Match:
    if not isfinite(wait_after_seconds) or wait_after_seconds < 0.0:
        raise ValueError("wait_after_seconds must be a finite non-negative number")
    found = find(
        window,
        target,
        threshold=threshold,
        timeout_seconds=timeout_seconds,
        overlap=overlap,
        retry_on_ambiguity=retry_on_ambiguity,
        target_scale_tolerance=target_scale_tolerance,
        target_scale_step=target_scale_step,
    )
    mouse.click(found.click, button=button)
    if wait_after_seconds:
        time.sleep(wait_after_seconds)
    return found


@reporting._trace("Find and click best target", hwnd_parameter="window")
def click_best(
    window: Window | HWND | int,
    target: Target,
    *,
    threshold: float = 0.90,
    timeout_seconds: float = 0.0,
    overlap: float = 0.30,
    target_scale_tolerance: float = 0.0,
    target_scale_step: float = 0.02,
    button: mouse.Button = "left",
    wait_after_seconds: float = _POST_CLICK_DELAY_SECONDS,
) -> Match:
    if not isfinite(wait_after_seconds) or wait_after_seconds < 0.0:
        raise ValueError("wait_after_seconds must be a finite non-negative number")
    found = find_best(
        window,
        target,
        threshold=threshold,
        timeout_seconds=timeout_seconds,
        overlap=overlap,
        target_scale_tolerance=target_scale_tolerance,
        target_scale_step=target_scale_step,
    )
    mouse.click(found.click, button=button)
    if wait_after_seconds:
        time.sleep(wait_after_seconds)
    return found
