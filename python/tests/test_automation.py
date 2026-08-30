from __future__ import annotations

import unittest
from unittest.mock import patch

from litewinwrap import Automation
from litewinwrap.types import HWND


class AutomationTests(unittest.TestCase):
    def test_validates_session_policy(self) -> None:
        invalid = (
            {"timeout_seconds": -1.0},
            {"timeout_seconds": float("inf")},
            {"settle_seconds": float("nan")},
            {"threshold": 1.1},
            {"overlap": 1.0},
            {"dpi_awareness": "system"},
        )
        for values in invalid:
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    Automation(**values)

    def test_dpi_awareness_can_be_left_to_the_host_process(self) -> None:
        with (
            patch("litewinwrap.automation.sys.platform", "win32"),
            patch(
                "litewinwrap.automation.win32.enable_per_monitor_dpi_awareness"
            ) as enable,
        ):
            Automation(dpi_awareness="unchanged")

        enable.assert_not_called()

    def test_windows_are_bound_to_the_creating_session(self) -> None:
        automation = Automation(timeout_seconds=4.0)
        with (
            patch("litewinwrap.automation.win32.enum_windows", return_value=(HWND(7),)),
            patch("litewinwrap.automation.win32.is_window_visible", return_value=True),
        ):
            windows = automation.windows()

        self.assertEqual(tuple(int(window.hwnd) for window in windows), (7,))
        self.assertIs(windows[0].automation, automation)


if __name__ == "__main__":
    unittest.main()
