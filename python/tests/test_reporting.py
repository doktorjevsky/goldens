from __future__ import annotations

import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from PIL import Image

from litewinwrap import Automation, TargetNotFoundError, keyboard
from litewinwrap.reporting import Reports, _action
from litewinwrap.types import Capture, HWND, Rect, Target


class ReportingTests(unittest.TestCase):
    def _capture(self) -> Capture:
        pixels = np.zeros((20, 30, 3), dtype=np.uint8)
        pixels[:, :] = (30, 80, 140)
        return Capture(pixels, Rect(100, 200, 130, 220))

    def test_failure_writes_one_static_report_for_the_test(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(
                temporary_directory,
                playback_frame_duration_seconds=0.1,
            )

            @reports.test(
                title="Save <document>",
                owner="Editor & tools",
                purpose="Exercise the save workflow",
            )
            def failing_test() -> None:
                with reports.step("Click Save"):
                    with _action(
                        "Click target",
                        hwnd=HWND(7),
                        details={"target": "toolbar/save", "point": [110, 210]},
                    ):
                        pass
                    raise RuntimeError("save failed")

            with patch("litewinwrap.match.capture", return_value=self._capture()):
                with self.assertRaisesRegex(RuntimeError, "save failed") as raised:
                    failing_test()

            report = reports.last_report
            self.assertIsNotNone(report)
            assert report is not None
            self.assertTrue(report.is_file())
            self.assertTrue((report.parent / "trace.json").is_file())
            self.assertTrue((report.parent / "failure.gif").is_file())
            self.assertTrue((report.parent / "failure.png").is_file())
            self.assertGreaterEqual(
                len(tuple((report.parent / "images").glob("frame-*.png"))),
                1,
            )
            with Image.open(report.parent / "failure.gif") as animation:
                self.assertGreaterEqual(animation.n_frames, 1)

            html = report.read_text(encoding="utf-8")
            self.assertIn("Save &lt;document&gt;", html)
            self.assertIn("Editor &amp; tools", html)
            self.assertNotIn("Rerunning may help", html)
            self.assertNotIn("Likely", html)
            self.assertNotIn("<h2>Steps</h2>", html)
            self.assertNotIn("<h2>Environment</h2>", html)
            self.assertIn("Technical details", html)
            self.assertIn("max-height: min(48vh, 380px)", html)
            self.assertTrue(
                any(
                    "litewinwrap report:" in note for note in raised.exception.__notes__
                )
            )

            trace = json.loads(
                (report.parent / "trace.json").read_text(encoding="utf-8")
            )
            self.assertEqual(trace["test"]["id"], "failing_test")
            self.assertEqual(trace["failure"]["type"], "RuntimeError")
            self.assertEqual(trace["steps"][0]["status"], "failed")
            self.assertEqual(trace["actions"][0]["action"], "Click target")

    def test_missing_target_image_is_prominent_in_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(temporary_directory)
            pixels = np.zeros((6, 8, 3), dtype=np.uint8)
            pixels[:, :] = (20, 120, 240)
            target = Target("dialog/submit", pixels)

            @reports.test(title="Find the submit button")
            def failing_test() -> None:
                raise TargetNotFoundError(
                    target,
                    threshold=0.92,
                    best_score=0.74,
                    elapsed_seconds=3.0,
                    attempts=12,
                    last_capture=self._capture(),
                )

            with self.assertRaises(TargetNotFoundError):
                failing_test()

            assert reports.last_report is not None
            directory = reports.last_report.parent
            with Image.open(directory / "target.png") as target_image:
                self.assertEqual(target_image.size, (8, 6))
            with Image.open(directory / "failure.gif") as animation:
                self.assertEqual(animation.info["duration"], 500)

            html = reports.last_report.read_text(encoding="utf-8")
            self.assertIn("Missing target", html)
            self.assertIn("dialog/submit", html)
            self.assertIn("src='target.png'", html)
            trace = json.loads((directory / "trace.json").read_text(encoding="utf-8"))
            self.assertEqual(
                trace["target_image"],
                {
                    "path": "target.png",
                    "name": "dialog/submit",
                    "width": 8,
                    "height": 6,
                },
            )

    def test_recording_cadence_is_independent_from_playback_speed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(
                temporary_directory,
                max_frames=20,
                recording_interval_seconds=0.01,
                playback_frame_duration_seconds=0.4,
            )
            capture_count = 0

            def changing_capture(_hwnd: HWND) -> Capture:
                nonlocal capture_count
                capture_count += 1
                capture_value = self._capture()
                pixels = capture_value.pixels.copy()
                pixels[0, 0] = (capture_count, 80, 140)
                return Capture(pixels, capture_value.rect)

            @reports.test(title="Wait for rendering")
            def failing_test() -> None:
                with reports.step("Open the dialog"):
                    with _action("Wait for UI", hwnd=HWND(7)):
                        time.sleep(0.07)
                    raise RuntimeError("dialog did not open")

            with (
                patch("litewinwrap.match.capture", side_effect=changing_capture),
                self.assertRaises(RuntimeError),
            ):
                failing_test()

            assert reports.last_report is not None
            trace = json.loads(
                (reports.last_report.parent / "trace.json").read_text(encoding="utf-8")
            )
            recorded_action_frames = [
                frame for frame in trace["frames"] if frame["action"] == "Wait for UI"
            ]
            self.assertGreaterEqual(len(recorded_action_frames), 3)
            with Image.open(reports.last_report.parent / "failure.gif") as animation:
                total_duration_milliseconds = 0
                for frame_index in range(animation.n_frames):
                    animation.seek(frame_index)
                    total_duration_milliseconds += animation.info["duration"]
                self.assertEqual(
                    total_duration_milliseconds,
                    len(trace["frames"]) * 400,
                )

    def test_report_capture_lock_is_reentrant_for_normal_window_capture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(temporary_directory)
            rect = Rect(100, 200, 130, 220)
            raw = np.zeros((rect.height, rect.width, 4), dtype=np.uint8).tobytes()

            @reports.test(title="Capture the active window")
            def failing_test() -> None:
                with _action("Read UI", hwnd=HWND(7)):
                    pass
                raise RuntimeError("expected failure")

            with (
                patch("litewinwrap.match.win32.is_window", return_value=True),
                patch(
                    "litewinwrap.match.win32.get_extended_frame_rect",
                    return_value=rect,
                ),
                patch("litewinwrap.match.win32.capture_screen", return_value=raw),
                self.assertRaises(RuntimeError),
            ):
                failing_test()

            assert reports.last_report is not None
            trace = json.loads(
                (reports.last_report.parent / "trace.json").read_text(encoding="utf-8")
            )
            self.assertGreaterEqual(len(trace["frames"]), 2)

    def test_success_does_not_write_an_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(temporary_directory)

            @reports.test(title="A passing test")
            def passing_test() -> int:
                with reports.step("Do the work"):
                    return 42

            self.assertEqual(passing_test(), 42)
            self.assertIsNone(reports.last_report)
            self.assertEqual(tuple(Path(temporary_directory).iterdir()), ())

    def test_typed_text_is_redacted_from_actions_and_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(temporary_directory, capture_frames=False)

            @reports.test(title="Text entry")
            def failing_test() -> None:
                keyboard.type_text("secret-value")
                raise AssertionError("expected failure")

            with (
                patch("litewinwrap.keyboard.win32.send_input", return_value=24),
                self.assertRaises(AssertionError),
            ):
                failing_test()

            assert reports.last_report is not None
            trace_text = (reports.last_report.parent / "trace.json").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("secret-value", trace_text)
            trace = json.loads(trace_text)
            type_action = next(
                item for item in trace["actions"] if item["action"] == "Type text"
            )
            self.assertEqual(type_action["details"]["characters"], 12)

    def test_report_generation_failure_never_masks_the_test_error(self) -> None:
        reports = Reports("unused")
        original = RuntimeError("original failure")

        @reports.test(title="Broken reporter")
        def failing_test() -> None:
            raise original

        with (
            patch.object(reports, "_write_failure", side_effect=OSError("disk full")),
            self.assertRaises(RuntimeError) as raised,
        ):
            failing_test()

        self.assertIs(raised.exception, original)
        self.assertTrue(any("disk full" in note for note in raised.exception.__notes__))

    def test_window_and_keyboard_actions_form_one_nested_trace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(temporary_directory, capture_frames=False)
            window = Automation(
                settle_seconds=0.0,
                focus_before_input=False,
            ).window(HWND(7))

            @reports.test(title="Keyboard shortcut")
            def failing_test() -> None:
                with reports.step("Close the window"):
                    window.press("ctrl", "q")
                    raise AssertionError("window remained open")

            with (
                patch("litewinwrap.keyboard.win32.send_input", return_value=4),
                self.assertRaises(AssertionError),
            ):
                failing_test()

            assert reports.last_report is not None
            trace = json.loads(
                (reports.last_report.parent / "trace.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                [item["action"] for item in trace["actions"]],
                ["Press keys in window", "Press keys"],
            )
            self.assertEqual(
                [item["depth"] for item in trace["actions"]],
                [0, 1],
            )
            self.assertEqual(trace["actions"][0]["details"]["keys"], ["ctrl", "q"])

    def test_step_requires_an_active_test(self) -> None:
        reports = Reports("unused")
        with self.assertRaisesRegex(RuntimeError, "inside @reports.test"):
            with reports.step("Outside"):
                pass

    def test_validates_time_and_size_configuration(self) -> None:
        for values in (
            {"max_frames": 0},
            {"recording_interval_seconds": 0.0},
            {"recording_interval_seconds": float("nan")},
            {"playback_frame_duration_seconds": 0.0},
            {"playback_frame_duration_seconds": float("nan")},
        ):
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    Reports("unused", **values)


if __name__ == "__main__":
    unittest.main()
