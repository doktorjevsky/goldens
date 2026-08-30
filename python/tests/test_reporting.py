from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from PIL import Image

from litewinwrap import Automation, keyboard
from litewinwrap.reporting import Reports, _action
from litewinwrap.types import Capture, HWND, Rect


class ReportingTests(unittest.TestCase):
    def _capture(self) -> Capture:
        pixels = np.zeros((20, 30, 3), dtype=np.uint8)
        pixels[:, :] = (30, 80, 140)
        return Capture(pixels, Rect(100, 200, 130, 220))

    def test_failure_writes_one_static_report_for_the_test(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            reports = Reports(
                temporary_directory,
                frame_duration_seconds=0.1,
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
            {"frame_duration_seconds": 0.0},
            {"frame_duration_seconds": float("nan")},
        ):
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    Reports("unused", **values)


if __name__ == "__main__":
    unittest.main()
