from __future__ import annotations

import inspect
import json
import os
import platform
import re
import sys
import threading
import time
import traceback
from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass, field
from datetime import datetime, timezone
from functools import wraps
from html import escape
from io import BytesIO
from math import isfinite
from pathlib import Path
from typing import Any, Literal, ParamSpec, TypeVar

import cv2
import numpy as np

from .types import Capture, HWND, Match, Point, Rect, Target


_P = ParamSpec("_P")
_R = TypeVar("_R")
_FALLBACK_GIF_FRAME_DURATION_SECONDS = 0.5

_CURRENT_RUN: ContextVar[_Run | None] = ContextVar(
    "litewinwrap_current_report_run",
    default=None,
)
_CURRENT_STEP_ID: ContextVar[int | None] = ContextVar(
    "litewinwrap_current_report_step",
    default=None,
)
_ACTION_DEPTH: ContextVar[int] = ContextVar(
    "litewinwrap_report_action_depth",
    default=0,
)
_CAPTURE_ENABLED: ContextVar[bool] = ContextVar(
    "litewinwrap_report_capture_enabled",
    default=True,
)


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _slug(value: str) -> str:
    result = re.sub(r"[^a-z0-9]+", "-", value.casefold()).strip("-")
    return result or "automation-test"


def _json_value(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, Mapping):
        return {str(key): _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_json_value(item) for item in value]
    return repr(value)


def _script_json(value: Any) -> str:
    return (
        json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        .replace("&", "\\u0026")
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("\u2028", "\\u2028")
        .replace("\u2029", "\\u2029")
    )


def _point_value(point: Point | tuple[int, int] | None) -> list[int] | None:
    return None if point is None else [int(point[0]), int(point[1])]


def _rect_value(rect: Rect | None) -> list[int] | None:
    return None if rect is None else [int(value) for value in rect]


@dataclass(slots=True)
class _Frame:
    png: bytes
    capture_rect: Rect
    action_id: int | None
    action: str
    step: str | None
    details: dict[str, Any]


@dataclass(slots=True)
class _Run:
    reports: Reports
    test_id: str
    title: str
    owner: str | None
    purpose: str | None
    source_file: str | None
    source_line: int | None
    capture_frames: bool
    started_at: datetime = field(default_factory=_utc_now)
    started_seconds: float = field(default_factory=time.monotonic)
    actions: list[dict[str, Any]] = field(default_factory=list)
    steps: list[dict[str, Any]] = field(default_factory=list)
    frames: list[_Frame] = field(default_factory=list)
    capture_errors: list[str] = field(default_factory=list)
    active_hwnd: HWND | None = None
    sampling_step_id: int | None = None
    sampling_capture_enabled: bool = True
    sampling_action_id: int | None = None
    sampling_action: str = "UI state"
    sampling_details: dict[str, Any] = field(default_factory=dict)
    _state_lock: Any = field(default_factory=threading.Lock, repr=False)
    _capture_lock: Any = field(default_factory=threading.RLock, repr=False)
    _next_action_id: int = 1
    _next_step_id: int = 1

    def elapsed_seconds(self) -> float:
        return time.monotonic() - self.started_seconds

    def next_action_id(self) -> int:
        value = self._next_action_id
        self._next_action_id += 1
        return value

    def next_step_id(self) -> int:
        value = self._next_step_id
        self._next_step_id += 1
        return value

    def step_title(self, step_id: int | None) -> str | None:
        if step_id is None:
            return None
        return next(
            (item["title"] for item in self.steps if item["id"] == step_id),
            None,
        )

    def begin_action_sampling(
        self,
        *,
        action_id: int,
        action: str,
        details: dict[str, Any],
        hwnd: HWND | None,
    ) -> None:
        with self._state_lock:
            if hwnd is not None:
                self.active_hwnd = hwnd
            self.sampling_action_id = action_id
            self.sampling_action = action
            self.sampling_details = dict(details)

    def finish_action_sampling(
        self,
        *,
        action_id: int,
        hwnd: HWND | None,
    ) -> None:
        with self._state_lock:
            if hwnd is not None:
                self.active_hwnd = hwnd
            if self.sampling_action_id == action_id:
                self.sampling_action_id = None
                self.sampling_action = "UI state"
                self.sampling_details = {}

    def enter_step_sampling(
        self,
        step_id: int,
        capture_enabled: bool,
    ) -> tuple[int | None, bool]:
        with self._state_lock:
            previous = (self.sampling_step_id, self.sampling_capture_enabled)
            self.sampling_step_id = step_id
            self.sampling_capture_enabled = capture_enabled
            return previous

    def restore_step_sampling(self, previous: tuple[int | None, bool]) -> None:
        with self._state_lock:
            self.sampling_step_id, self.sampling_capture_enabled = previous

    def sampling_snapshot(
        self,
    ) -> tuple[HWND | None, int | None, str, str | None, dict[str, Any], bool]:
        with self._state_lock:
            return (
                self.active_hwnd,
                self.sampling_action_id,
                self.sampling_action,
                self.step_title(self.sampling_step_id),
                dict(self.sampling_details),
                self.sampling_capture_enabled,
            )

    def capture(
        self,
        *,
        action_id: int | None,
        action: str,
        details: dict[str, Any],
        capture_value: Capture | None = None,
        capture_enabled: bool | None = None,
        step: str | None = None,
    ) -> None:
        if capture_enabled is None:
            capture_enabled = _CAPTURE_ENABLED.get()
        if not self.capture_frames or not capture_enabled:
            return
        with self._capture_lock:
            try:
                if capture_value is None:
                    if self.active_hwnd is None:
                        return
                    from . import match as image_match

                    capture_value = image_match.capture(self.active_hwnd)
                success, encoded = cv2.imencode(
                    ".png",
                    np.ascontiguousarray(capture_value.pixels),
                )
                if not success:
                    raise OSError("OpenCV could not encode the captured frame")
                if step is None:
                    step = self.step_title(_CURRENT_STEP_ID.get())
                self.frames.append(
                    _Frame(
                        png=encoded.tobytes(),
                        capture_rect=capture_value.rect,
                        action_id=action_id,
                        action=action,
                        step=step,
                        details=dict(details),
                    )
                )
                if len(self.frames) > self.reports.max_frames:
                    del self.frames[: len(self.frames) - self.reports.max_frames]
            except Exception as error:
                message = f"{type(error).__name__}: {error}"
                if not self.capture_errors or self.capture_errors[-1] != message:
                    self.capture_errors.append(message)


@contextmanager
def _capture_guard() -> Iterator[None]:
    """Serialize automation and report captures while a report is active."""

    run = _CURRENT_RUN.get()
    if run is None:
        yield
        return
    with run._capture_lock:
        yield


class _FrameSampler:
    def __init__(self, run: _Run, recording_interval_seconds: float) -> None:
        self._run = run
        self._recording_interval_seconds = recording_interval_seconds
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._sample,
            name=f"litewinwrap-report-{run.test_id}",
            daemon=True,
        )
        self._started = False

    def start(self) -> None:
        if not self._run.capture_frames:
            return
        try:
            self._thread.start()
        except Exception as error:
            self._run.capture_errors.append(
                f"Frame recorder: {type(error).__name__}: {error}"
            )
        else:
            self._started = True

    def stop(self) -> None:
        if not self._started:
            return
        self._stop_event.set()
        self._thread.join()
        self._started = False

    def _sample(self) -> None:
        while not self._stop_event.wait(self._recording_interval_seconds):
            (
                hwnd,
                action_id,
                action,
                step,
                details,
                capture_enabled,
            ) = self._run.sampling_snapshot()
            if hwnd is None or not capture_enabled:
                continue
            self._run.capture(
                action_id=action_id,
                action=action,
                details=details,
                capture_enabled=True,
                step=step,
            )


class _ActionSpan:
    __slots__ = ("_capture", "_run", "details", "hwnd")

    def __init__(
        self,
        run: _Run | None,
        *,
        hwnd: HWND | int | None,
    ) -> None:
        self._run = run
        self._capture: Capture | None = None
        self.details: dict[str, Any] = {}
        self.hwnd = HWND(int(hwnd)) if hwnd is not None else None

    def add(self, **details: Any) -> None:
        self.details.update(details)

    def set_hwnd(self, hwnd: HWND | int) -> None:
        self.hwnd = HWND(int(hwnd))

    def attach_capture(self, capture_value: Capture) -> None:
        self._capture = capture_value


def _trace(
    action: str,
    *,
    hwnd_parameter: str | None = None,
    capture: bool = True,
) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
    """Instrument one synchronous public operation when a report is active."""

    def decorate(function: Callable[_P, _R]) -> Callable[_P, _R]:
        signature = inspect.signature(function)

        @wraps(function)
        def wrapped(*args: _P.args, **kwargs: _P.kwargs) -> _R:
            if _CURRENT_RUN.get() is None:
                return function(*args, **kwargs)
            bound = signature.bind_partial(*args, **kwargs)
            details = _argument_details(bound.arguments)
            hwnd: HWND | int | None = None
            if args and hasattr(args[0], "hwnd"):
                hwnd = getattr(args[0], "hwnd")
            if hwnd_parameter is not None:
                candidate = bound.arguments.get(hwnd_parameter)
                if hasattr(candidate, "hwnd"):
                    hwnd = getattr(candidate, "hwnd")
                elif isinstance(candidate, int):
                    hwnd = candidate

            with _action(
                action,
                hwnd=hwnd,
                details=details,
                capture=capture,
            ) as span:
                for value in bound.arguments.values():
                    if isinstance(value, Capture):
                        span.attach_capture(value)
                        break
                result = function(*args, **kwargs)
                result_details, result_capture, result_hwnd = _result_details(result)
                span.add(**result_details)
                if result_capture is not None:
                    span.attach_capture(result_capture)
                if result_hwnd is not None:
                    span.set_hwnd(result_hwnd)
                return result

        return wrapped

    return decorate


def _argument_details(arguments: Mapping[str, Any]) -> dict[str, Any]:
    details: dict[str, Any] = {}
    for name, value in arguments.items():
        if name in {"self", "cls"}:
            continue
        if name == "text" and isinstance(value, str):
            details["characters"] = len(value)
            continue
        if isinstance(value, Target):
            height, width = value.pixels.shape[:2]
            details["target"] = value.name
            details["target_size"] = [width, height]
            if value.click is not None:
                details["target_click"] = list(value.click)
            continue
        if isinstance(value, Capture):
            details["capture_rect"] = _rect_value(value.rect)
            continue
        if isinstance(value, Point):
            details[name] = _point_value(value)
            continue
        if isinstance(value, Rect):
            details[name] = _rect_value(value)
            continue
        if name in {"point", "destination", "origin"} and value is not None:
            details[name] = _point_value(value)
            continue
        details[name] = _json_value(value)
    return details


def _result_details(
    result: Any,
) -> tuple[dict[str, Any], Capture | None, HWND | None]:
    if isinstance(result, Capture):
        return {"capture_rect": _rect_value(result.rect)}, result, None
    if isinstance(result, Match):
        return (
            {
                "match_score": result.score,
                "match_rect": _rect_value(result.rect),
                "point": _point_value(result.click),
            },
            None,
            None,
        )
    if isinstance(result, Rect):
        return {"result_rect": _rect_value(result)}, None, None
    if hasattr(result, "hwnd"):
        return {"hwnd": int(result.hwnd)}, None, HWND(int(result.hwnd))
    if isinstance(result, tuple):
        details: dict[str, Any] = {"result_count": len(result)}
        if result and all(isinstance(item, Match) for item in result):
            details["matches"] = [
                {
                    "score": item.score,
                    "rect": _rect_value(item.rect),
                    "click": _point_value(item.click),
                }
                for item in result
            ]
        return details, None, None
    if isinstance(result, int) and not isinstance(result, bool):
        return {"events_sent": result}, None, None
    return {}, None, None


@contextmanager
def _action(
    action: str,
    *,
    hwnd: HWND | int | None = None,
    details: Mapping[str, Any] | None = None,
    capture: bool = True,
) -> Iterator[_ActionSpan]:
    run = _CURRENT_RUN.get()
    span = _ActionSpan(run, hwnd=hwnd)
    if details:
        span.add(**dict(details))
    if run is None:
        yield span
        return

    action_id = run.next_action_id()
    depth = _ACTION_DEPTH.get()
    depth_token = _ACTION_DEPTH.set(depth + 1)
    started_seconds = run.elapsed_seconds()
    started_clock = time.monotonic()
    if depth == 0:
        run.begin_action_sampling(
            action_id=action_id,
            action=action,
            details=span.details,
            hwnd=span.hwnd,
        )
    status = "passed"
    error_type: str | None = None
    error_message: str | None = None
    try:
        yield span
    except Exception as error:
        status = "failed"
        error_type = type(error).__name__
        error_message = str(error)
        last_capture = getattr(error, "last_capture", None)
        if isinstance(last_capture, Capture):
            span.attach_capture(last_capture)
        raise
    finally:
        duration_seconds = time.monotonic() - started_clock
        _ACTION_DEPTH.reset(depth_token)
        event = {
            "id": action_id,
            "action": action,
            "step_id": _CURRENT_STEP_ID.get(),
            "depth": depth,
            "status": status,
            "started_seconds": started_seconds,
            "duration_seconds": duration_seconds,
            "details": _json_value(span.details),
        }
        if error_type is not None:
            event["error"] = {
                "type": error_type,
                "message": error_message,
            }
        run.actions.append(event)
        if capture and depth == 0:
            run.capture(
                action_id=action_id,
                action=action,
                details=span.details,
                capture_value=span._capture,
            )
        if depth == 0:
            run.finish_action_sampling(
                action_id=action_id,
                hwnd=span.hwnd,
            )


class Reports:
    """Configure per-test static failure reports for automation scripts."""

    def __init__(
        self,
        artifact_directory: str | os.PathLike[str],
        *,
        retain: Literal["failures"] = "failures",
        capture_frames: bool = True,
        max_frames: int = 40,
        recording_interval_seconds: float = 0.2,
    ) -> None:
        if retain != "failures":
            raise ValueError("The MVP supports retain='failures' only")
        if not isinstance(capture_frames, bool):
            raise TypeError("capture_frames must be a boolean")
        if max_frames <= 0:
            raise ValueError("max_frames must be positive")
        if (
            not isfinite(recording_interval_seconds)
            or recording_interval_seconds <= 0.0
        ):
            raise ValueError(
                "recording_interval_seconds must be a finite positive number"
            )
        self.artifact_directory = Path(artifact_directory)
        self.retain = retain
        self.capture_frames = capture_frames
        self.max_frames = max_frames
        self.recording_interval_seconds = float(recording_interval_seconds)
        self.last_report: Path | None = None

    def test(
        self,
        *,
        title: str,
        owner: str | None = None,
        purpose: str | None = None,
        test_id: str | None = None,
        capture_frames: bool | None = None,
    ) -> Callable[[Callable[_P, _R]], Callable[_P, _R]]:
        """Decorate one independently reported automation test."""

        if not title.strip():
            raise ValueError("A report test title is required")
        if capture_frames is not None and not isinstance(capture_frames, bool):
            raise TypeError("capture_frames must be a boolean or None")

        def decorate(function: Callable[_P, _R]) -> Callable[_P, _R]:
            source_file = inspect.getsourcefile(function)
            try:
                source_line = inspect.getsourcelines(function)[1]
            except (OSError, TypeError):
                source_line = None
            resolved_test_id = test_id or function.__name__

            @wraps(function)
            def wrapped(*args: _P.args, **kwargs: _P.kwargs) -> _R:
                if _CURRENT_RUN.get() is not None:
                    raise RuntimeError("Reported tests cannot be nested")
                run = _Run(
                    reports=self,
                    test_id=resolved_test_id,
                    title=title,
                    owner=owner,
                    purpose=purpose,
                    source_file=source_file,
                    source_line=source_line,
                    capture_frames=(
                        self.capture_frames
                        if capture_frames is None
                        else capture_frames
                    ),
                )
                run_token = _CURRENT_RUN.set(run)
                step_token = _CURRENT_STEP_ID.set(None)
                depth_token = _ACTION_DEPTH.set(0)
                capture_token = _CAPTURE_ENABLED.set(True)
                sampler = _FrameSampler(run, self.recording_interval_seconds)
                sampler.start()
                try:
                    return function(*args, **kwargs)
                except Exception as error:
                    sampler.stop()
                    if not run.actions or run.actions[-1]["status"] != "failed":
                        run.capture(
                            action_id=None,
                            action="Test failure",
                            details={},
                            capture_value=(
                                getattr(error, "last_capture", None)
                                if isinstance(
                                    getattr(error, "last_capture", None), Capture
                                )
                                else None
                            ),
                        )
                    try:
                        report = self._write_failure(run, error)
                    except Exception as report_error:
                        error.add_note(
                            "litewinwrap could not write its failure report: "
                            f"{type(report_error).__name__}: {report_error}"
                        )
                    else:
                        self.last_report = report
                        error.add_note(f"litewinwrap report: {report}")
                        print(f"litewinwrap report: {report}", file=sys.stderr)
                    raise
                finally:
                    sampler.stop()
                    _CAPTURE_ENABLED.reset(capture_token)
                    _ACTION_DEPTH.reset(depth_token)
                    _CURRENT_STEP_ID.reset(step_token)
                    _CURRENT_RUN.reset(run_token)

            return wrapped

        return decorate

    @contextmanager
    def step(
        self,
        title: str,
        *,
        capture_frames: bool | None = None,
    ) -> Iterator[None]:
        """Add a human-readable step to the active reported test."""

        run = _CURRENT_RUN.get()
        if run is None or run.reports is not self:
            raise RuntimeError("reports.step() must run inside @reports.test")
        if not title.strip():
            raise ValueError("A report step title is required")
        if capture_frames is not None and not isinstance(capture_frames, bool):
            raise TypeError("capture_frames must be a boolean or None")

        step_id = run.next_step_id()
        step = {
            "id": step_id,
            "title": title,
            "status": "running",
            "started_seconds": run.elapsed_seconds(),
            "duration_seconds": None,
        }
        run.steps.append(step)
        started_seconds = time.monotonic()
        step_token = _CURRENT_STEP_ID.set(step_id)
        capture_token = None
        if capture_frames is not None:
            capture_token = _CAPTURE_ENABLED.set(capture_frames)
        sampling_state = run.enter_step_sampling(
            step_id,
            _CAPTURE_ENABLED.get(),
        )
        try:
            yield
        except Exception:
            step["status"] = "failed"
            raise
        else:
            step["status"] = "passed"
        finally:
            step["duration_seconds"] = time.monotonic() - started_seconds
            run.restore_step_sampling(sampling_state)
            if capture_token is not None:
                _CAPTURE_ENABLED.reset(capture_token)
            _CURRENT_STEP_ID.reset(step_token)

    def _write_failure(self, run: _Run, error: Exception) -> Path:
        timestamp = run.started_at.strftime("%Y%m%d-%H%M%S-%fZ")
        directory = self.artifact_directory / f"{_slug(run.test_id)}--{timestamp}"
        images_directory = directory / "images"
        images_directory.mkdir(parents=True, exist_ok=False)

        media = _write_media(
            run.frames,
            directory=directory,
            images_directory=images_directory,
        )
        try:
            media["target"] = _write_target(error, directory)
        except Exception as target_error:
            media["target"] = None
            run.capture_errors.append(
                f"Target image: {type(target_error).__name__}: {target_error}"
            )
        finished_at = _utc_now()
        trace = {
            "schema_version": 1,
            "status": "failed",
            "test": {
                "id": run.test_id,
                "title": run.title,
                "owner": run.owner,
                "purpose": run.purpose,
                "source_file": run.source_file,
                "source_line": run.source_line,
            },
            "started_at": run.started_at.isoformat(),
            "finished_at": finished_at.isoformat(),
            "duration_seconds": run.elapsed_seconds(),
            "failure": _failure_details(error),
            "steps": run.steps,
            "actions": sorted(
                run.actions,
                key=lambda item: item["started_seconds"],
            ),
            "frames": media["frames"],
            "target_image": media["target"],
            "capture_errors": run.capture_errors,
            "environment": {
                "python": sys.version,
                "platform": platform.platform(),
                "executable": sys.executable,
                "working_directory": os.getcwd(),
                "litewinwrap": _package_version(),
            },
        }
        (directory / "trace.json").write_text(
            json.dumps(trace, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        (directory / "index.html").write_text(
            _render_html(trace, media),
            encoding="utf-8",
        )
        return directory / "index.html"


def _package_version() -> str:
    package = sys.modules.get("litewinwrap")
    return str(getattr(package, "__version__", "unknown"))


def _failure_details(error: Exception) -> dict[str, Any]:
    details: dict[str, Any] = {
        "type": type(error).__name__,
        "message": str(error),
        "traceback": "".join(
            traceback.format_exception(type(error), error, error.__traceback__)
        ),
    }
    target = getattr(error, "target", None)
    if target is not None:
        details["target"] = getattr(target, "name", repr(target))
    for name in (
        "threshold",
        "best_score",
        "elapsed_seconds",
        "attempts",
    ):
        if hasattr(error, name):
            details[name] = _json_value(getattr(error, name))
    matches = getattr(error, "matches", None)
    if matches is not None:
        details["matches"] = [
            {
                "score": item.score,
                "rect": _rect_value(item.rect),
                "click": _point_value(item.click),
            }
            if isinstance(item, Match)
            else repr(item)
            for item in matches
        ]
    windows = getattr(error, "windows", None)
    if windows is not None:
        details["window_handles"] = [int(window.hwnd) for window in windows]
    return details


def _write_target(error: Exception, directory: Path) -> dict[str, Any] | None:
    target = getattr(error, "target", None)
    if not isinstance(target, Target):
        return None
    success, encoded = cv2.imencode(
        ".png",
        np.ascontiguousarray(target.pixels),
    )
    if not success:
        raise OSError("OpenCV could not encode the target image")
    filename = "target.png"
    (directory / filename).write_bytes(encoded.tobytes())
    height, width = target.pixels.shape[:2]
    return {
        "path": filename,
        "name": target.name,
        "width": width,
        "height": height,
    }


def _write_media(
    frames: list[_Frame],
    *,
    directory: Path,
    images_directory: Path,
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "gif": None,
        "final_image": None,
        "target": None,
        "frames": [],
    }
    if not frames:
        return result

    from PIL import Image, ImageDraw, ImageFont

    try:
        label_font = ImageFont.truetype("DejaVuSans.ttf", 16)
    except OSError:
        label_font = ImageFont.load_default()

    prepared: list[Image.Image] = []
    for index, frame in enumerate(frames, start=1):
        filename = f"frame-{index:03d}.png"
        (images_directory / filename).write_bytes(frame.png)
        result["frames"].append(
            {
                "path": f"images/{filename}",
                "action_id": frame.action_id,
                "action": frame.action,
                "step": frame.step,
            }
        )
        image = Image.open(BytesIO(frame.png)).convert("RGB")
        draw = ImageDraw.Draw(image)
        match_rect = frame.details.get("match_rect")
        if isinstance(match_rect, (list, tuple)) and len(match_rect) == 4:
            left, top, right, bottom = (int(value) for value in match_rect)
            draw.rectangle(
                (
                    left - frame.capture_rect.left,
                    top - frame.capture_rect.top,
                    right - frame.capture_rect.left,
                    bottom - frame.capture_rect.top,
                ),
                outline=(245, 158, 11),
                width=3,
            )
        point = frame.details.get("point")
        if isinstance(point, (list, tuple)) and len(point) == 2:
            x = int(point[0]) - frame.capture_rect.left
            y = int(point[1]) - frame.capture_rect.top
            draw.ellipse((x - 8, y - 8, x + 8, y + 8), outline=(239, 68, 68), width=4)
        image.thumbnail((960, 640), Image.Resampling.LANCZOS)
        prepared.append(image)

    width = max(image.width for image in prepared)
    height = max(image.height for image in prepared)
    rendered: list[Image.Image] = []
    for image, frame in zip(prepared, frames, strict=True):
        canvas = Image.new("RGB", (width, height + 44), (17, 24, 39))
        x = (width - image.width) // 2
        y = (height - image.height) // 2
        canvas.paste(image, (x, y))
        label = f"{frame.step} · {frame.action}" if frame.step else frame.action
        ImageDraw.Draw(canvas).text(
            (12, height + 12),
            label,
            fill=(241, 245, 249),
            font=label_font,
        )
        rendered.append(canvas)

    final_image = directory / "failure.png"
    rendered[-1].save(final_image, format="PNG")
    palette_samples: list[Image.Image] = []
    for image in rendered:
        sample = image.copy()
        sample.thumbnail((240, 160), Image.Resampling.LANCZOS)
        palette_samples.append(sample)
    palette_sheet = Image.new(
        "RGB",
        (
            max(image.width for image in palette_samples),
            sum(image.height for image in palette_samples),
        ),
    )
    sample_y = 0
    for image in palette_samples:
        palette_sheet.paste(image, (0, sample_y))
        sample_y += image.height
    palette = palette_sheet.quantize(
        colors=128,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    gif_frames = [
        image.quantize(palette=palette, dither=Image.Dither.NONE) for image in rendered
    ]
    gif = directory / "failure.gif"
    gif_frames[0].save(
        gif,
        format="GIF",
        save_all=True,
        append_images=gif_frames[1:],
        duration=max(1, round(_FALLBACK_GIF_FRAME_DURATION_SECONDS * 1000)),
        loop=0,
        disposal=1,
        optimize=True,
    )
    result["gif"] = gif.name
    result["final_image"] = final_image.name
    return result


def _render_html(trace: dict[str, Any], media: dict[str, Any]) -> str:
    test = trace["test"]
    failure = trace["failure"]
    step_names = {item["id"]: item["title"] for item in trace["steps"]}
    failed_step = next(
        (
            item["title"]
            for item in reversed(trace["steps"])
            if item["status"] == "failed"
        ),
        None,
    )
    context = [
        value
        for value in (
            f"Step: {failed_step}" if failed_step else None,
            f"Owner: {test['owner']}" if test.get("owner") else None,
            f"{trace['duration_seconds']:.2f}s",
        )
        if value is not None
    ]
    context_html = "".join(f"<span>{escape(value)}</span>" for value in context)

    evidence_items: list[str] = []
    target_media = media.get("target")
    if target_media:
        display_width = min(max(int(target_media["width"]) * 2, 96), 480)
        target_label = (
            "Missing target" if failure["type"] == "TargetNotFoundError" else "Target"
        )
        evidence_items.append(
            "<figure><figcaption>"
            f"{target_label}<strong>{escape(target_media['name'])}</strong>"
            "</figcaption><div class='target-stage'>"
            f"<a class='media-link target-link' href='{escape(target_media['path'])}' "
            "target='_blank' rel='noopener' title='Open full-size target'>"
            f"<img class='target' src='{escape(target_media['path'])}' "
            f"style='width:{display_width}px' alt='Target that was not found'>"
            "</a></div></figure>"
        )
    if media.get("final_image"):
        final_label = "Screen when matching failed" if target_media else "Final frame"
        evidence_items.append(
            "<figure><figcaption>"
            f"{final_label}"
            "</figcaption>"
            f"<a class='media-link' href='{escape(media['final_image'])}' "
            "target='_blank' rel='noopener' title='Open full-size image'>"
            f"<img class='visual' src='{escape(media['final_image'])}' "
            "alt='Screen captured when the test failed'></a></figure>"
        )
    evidence_html = ""
    if evidence_items:
        evidence_html = (
            "<section><div class='evidence'>"
            + "".join(evidence_items)
            + "</div></section>"
        )

    sequence_html = ""
    player_script = ""
    if media.get("frames"):
        frames = media["frames"]
        first_frame = frames[0]
        first_label = (
            f"{first_frame['step']} · {first_frame['action']}"
            if first_frame.get("step")
            else first_frame["action"]
        )
        disabled = " disabled" if len(frames) == 1 else ""
        sequence_html = (
            "<section class='sequence'>"
            "<div class='section-heading'><h2>What happened</h2>"
            f"<a href='{escape(media['gif'])}' target='_blank' rel='noopener'>"
            "GIF file</a></div>"
            f"<a id='sequence-link' class='media-link' href='{escape(first_frame['path'])}' "
            "target='_blank' rel='noopener' title='Open current frame full size'>"
            f"<img id='sequence-image' class='visual' src='{escape(first_frame['path'])}' "
            f"alt='Recorded frame 1 of {len(frames)}'></a>"
            "<div class='player-controls'>"
            f"<button id='previous-frame' type='button'{disabled}>Previous</button>"
            f"<button id='toggle-playback' type='button'{disabled}>Pause</button>"
            f"<button id='next-frame' type='button'{disabled}>Next</button>"
            f"<input id='frame-slider' type='range' min='0' max='{len(frames) - 1}' "
            f"value='0' aria-label='Current frame'{disabled}>"
            f"<output id='frame-position'>1 / {len(frames)}</output>"
            "<label>Speed <select id='playback-speed'>"
            "<option value='0.25'>0.25×</option>"
            "<option value='0.5'>0.5×</option>"
            "<option value='1' selected>1×</option>"
            "<option value='1.5'>1.5×</option>"
            "<option value='2'>2×</option>"
            "</select></label></div>"
            f"<p id='sequence-label' class='frame-label'>{escape(first_label)}</p>"
            f"<noscript><img class='visual' src='{escape(media['gif'])}' "
            "alt='Recorded action sequence'></noscript></section>"
        )
        player_script = _render_player_script(frames)
    elif media.get("gif"):
        sequence_html = (
            "<section class='sequence'><h2>What happened</h2>"
            f"<img class='visual' src='{escape(media['gif'])}' "
            "alt='Recorded action sequence'></section>"
        )

    action_items = "".join(
        "<li>"
        f"<span>{escape(step_names.get(item.get('step_id'), item['action']))}</span>"
        f"<strong>{escape(item['action'])}</strong>"
        f"<small>{float(item['started_seconds']):.2f}s · {escape(item['status'])}</small>"
        "</li>"
        for item in trace["actions"]
        if int(item["depth"]) == 0
    )
    source = test.get("source_file")
    if source and test.get("source_line"):
        source = f"{source}:{test['source_line']}"
    technical_facts = _fact_grid(
        [
            ("Exception", failure["type"]),
            ("Source", source),
            ("Purpose", test.get("purpose")),
            ("Started", trace["started_at"]),
        ]
    )
    capture_errors = ""
    if trace["capture_errors"]:
        capture_errors = (
            "<h3>Capture errors</h3><pre>"
            + escape("\n".join(trace["capture_errors"]))
            + "</pre>"
        )

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{escape(test["title"])} — failed</title>
<style>
:root {{ color-scheme: light; font-family: Inter, ui-sans-serif, system-ui, sans-serif; }}
body {{ margin: 0; background: #f4f5f7; color: #172033; }}
main {{ max-width: 900px; margin: 0 auto; padding: 24px 18px 48px; }}
header, section, details.technical {{ background: white; border: 1px solid #dbe2ea; border-radius: 10px; padding: 16px; margin-bottom: 12px; }}
h1 {{ margin: 8px 0 10px; font-size: 26px; }} h2 {{ margin: 0 0 14px; font-size: 17px; }} h3 {{ font-size: 14px; }}
.badge {{ display: inline-block; color: #991b1b; background: #fee2e2; border-radius: 999px; padding: 5px 10px; font-weight: 700; font-size: 12px; letter-spacing: .05em; }}
.message {{ margin: 0; font-size: 16px; line-height: 1.5; }}
.context {{ display: flex; flex-wrap: wrap; gap: 8px 18px; margin: 14px 0 0; color: #64748b; font-size: 13px; }}
.evidence {{ display: grid; grid-template-columns: minmax(150px, .8fr) minmax(0, 1.2fr); gap: 16px; align-items: start; }}
figure {{ margin: 0; min-width: 0; }} figcaption {{ margin-bottom: 10px; color: #475569; font-size: 13px; }} figcaption strong {{ display: block; margin-top: 3px; color: #172033; overflow-wrap: anywhere; }}
.target-stage {{ display: grid; min-height: 110px; max-height: 180px; place-items: center; padding: 12px; background: #eef1f5; border: 1px solid #cbd5e1; border-radius: 7px; overflow: hidden; }}
.target {{ display: block; max-width: 100%; max-height: 150px; height: auto; object-fit: contain; image-rendering: pixelated; }}
.facts {{ display: grid; grid-template-columns: minmax(120px, 180px) 1fr; gap: 8px 18px; }}
.facts dt {{ color: #64748b; font-weight: 600; }} .facts dd {{ margin: 0; overflow-wrap: anywhere; }}
.media-link {{ display: block; color: inherit; }} .target-link {{ display: grid; place-items: center; max-width: 100%; }}
.visual {{ display: block; max-width: 100%; height: auto; border: 1px solid #cbd5e1; border-radius: 7px; object-fit: contain; }}
.evidence .visual {{ width: auto; max-height: 280px; margin: 0 auto; }}
.section-heading {{ display: flex; align-items: baseline; justify-content: space-between; gap: 16px; }} .section-heading a {{ color: #475569; font-size: 13px; }}
.sequence .media-link {{ display: flex; justify-content: center; }} .sequence .visual {{ width: auto; max-height: min(48vh, 380px); }}
.player-controls {{ display: grid; grid-template-columns: auto auto auto minmax(100px, 1fr) auto auto; align-items: center; gap: 8px; margin-top: 12px; }}
.player-controls button, .player-controls select {{ min-height: 32px; border: 1px solid #cbd5e1; border-radius: 6px; background: white; color: #172033; }} .player-controls button {{ padding: 4px 10px; cursor: pointer; }} .player-controls button:disabled {{ cursor: default; opacity: .45; }}
.player-controls input {{ width: 100%; }} .player-controls output, .player-controls label {{ color: #475569; font-size: 13px; white-space: nowrap; }}
.frame-label {{ min-height: 1.3em; margin: 8px 0 0; color: #64748b; font-size: 13px; text-align: center; }}
.actions {{ margin: 18px 0; padding-left: 24px; }} .actions li {{ padding: 5px 0; }} .actions span, .actions strong, .actions small {{ display: block; }} .actions span, .actions small {{ color: #64748b; }}
pre {{ white-space: pre-wrap; overflow-wrap: anywhere; background: #0f172a; color: #e2e8f0; border-radius: 7px; padding: 14px; overflow: auto; }}
summary {{ cursor: pointer; color: #334155; font-weight: 600; }}
@media (max-width: 680px) {{ .evidence {{ grid-template-columns: 1fr; }} .player-controls {{ grid-template-columns: repeat(3, 1fr); }} .player-controls input {{ grid-column: 1 / 3; }} }}
</style>
</head>
<body><main>
<header><span class="badge">FAILED</span><h1>{escape(test["title"])}</h1><p class="message"><strong>{escape(failure["type"])}</strong>: {escape(failure["message"])}</p><p class="context">{context_html}</p></header>
{evidence_html}{sequence_html}
<details class="technical"><summary>Technical details</summary>{technical_facts}<h2>Actions</h2><ol class="actions">{action_items}</ol><h2>Traceback</h2><pre>{escape(failure["traceback"])}</pre>{capture_errors}</details>
</main>{player_script}</body></html>"""


def _render_player_script(
    frames: list[dict[str, Any]],
) -> str:
    default_frame_duration_milliseconds = round(
        _FALLBACK_GIF_FRAME_DURATION_SECONDS * 1000
    )
    return f"""<script>
(() => {{
  const frames = {_script_json(frames)};
  const defaultFrameDurationMilliseconds = {default_frame_duration_milliseconds};
  const image = document.getElementById("sequence-image");
  const link = document.getElementById("sequence-link");
  const label = document.getElementById("sequence-label");
  const slider = document.getElementById("frame-slider");
  const position = document.getElementById("frame-position");
  const toggle = document.getElementById("toggle-playback");
  const speedSelect = document.getElementById("playback-speed");
  let frameIndex = 0;
  let speed = 1;
  let playing = frames.length > 1;
  let timer = null;

  for (const frame of frames) {{
    const preload = new Image();
    preload.src = frame.path;
  }}

  function render() {{
    const frame = frames[frameIndex];
    image.src = frame.path;
    image.alt = `Recorded frame ${{frameIndex + 1}} of ${{frames.length}}`;
    link.href = frame.path;
    slider.value = String(frameIndex);
    position.textContent = `${{frameIndex + 1}} / ${{frames.length}}`;
    label.textContent = frame.step
      ? `${{frame.step}} · ${{frame.action}}`
      : frame.action;
  }}

  function schedule() {{
    window.clearTimeout(timer);
    if (!playing) return;
    timer = window.setTimeout(() => {{
      frameIndex = (frameIndex + 1) % frames.length;
      render();
      schedule();
    }}, defaultFrameDurationMilliseconds / speed);
  }}

  function show(frame) {{
    frameIndex = (frame + frames.length) % frames.length;
    render();
    schedule();
  }}

  document.getElementById("previous-frame").addEventListener("click", () => {{
    show(frameIndex - 1);
  }});
  document.getElementById("next-frame").addEventListener("click", () => {{
    show(frameIndex + 1);
  }});
  toggle.addEventListener("click", () => {{
    playing = !playing;
    toggle.textContent = playing ? "Pause" : "Play";
    schedule();
  }});
  slider.addEventListener("input", () => {{
    show(Number(slider.value));
  }});
  speedSelect.addEventListener("change", () => {{
    speed = Number(speedSelect.value);
    schedule();
  }});

  render();
  schedule();
}})();
</script>"""


def _fact_grid(items: list[tuple[str, Any]]) -> str:
    values = "".join(
        f"<dt>{escape(str(label))}</dt><dd>{escape(str(value))}</dd>"
        for label, value in items
        if value is not None and value != ""
    )
    return f"<dl class='facts'>{values}</dl>"
