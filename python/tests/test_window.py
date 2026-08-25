from __future__ import annotations

import unittest
from unittest.mock import patch

import numpy as np

from litewinwrap.types import Capture, HWND, Match, Point, Rect, Target
from litewinwrap.window import Window


class WindowTests(unittest.TestCase):
    def test_title_is_queried_on_every_access(self) -> None:
        window = Window(HWND(123))
        with patch(
            "litewinwrap.window.win32.get_window_text",
            side_effect=["first", "second"],
        ) as get_window_text:
            self.assertEqual(window.title, "first")
            self.assertEqual(window.title, "second")

        self.assertEqual(get_window_text.call_count, 2)
        self.assertFalse(hasattr(window, "__dict__"))

    def test_direct_children_are_filtered_from_descendants(self) -> None:
        window = Window(HWND(100))
        with (
            patch(
                "litewinwrap.window.win32.enum_child_windows",
                return_value=(HWND(101), HWND(102), HWND(103)),
            ),
            patch(
                "litewinwrap.window.win32.get_parent",
                side_effect=lambda hwnd: HWND(100) if hwnd != 103 else HWND(102),
            ),
        ):
            children = window.get_children(recursive=False, visible_only=False)

        self.assertEqual(tuple(int(child.hwnd) for child in children), (101, 102))

    def test_target_methods_delegate_with_the_window_handle(self) -> None:
        window = Window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        capture = Capture(np.zeros((4, 4, 3), dtype=np.uint8), Rect(1, 2, 5, 6))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with (
            patch("litewinwrap.match.capture", return_value=capture) as screenshot,
            patch("litewinwrap.match.find", return_value=found) as find,
            patch("litewinwrap.match.find_best", return_value=found) as find_best,
            patch("litewinwrap.match.find_all", return_value=(found,)) as find_all,
            patch("litewinwrap.match.click", return_value=found) as click,
            patch("litewinwrap.match.click_best", return_value=found) as click_best,
        ):
            self.assertIs(window.screenshot(), capture)
            self.assertIs(window.find_target(target, timeout=2.0), found)
            self.assertIs(window.find_best_target(target, timeout=2.0), found)
            self.assertEqual(window.find_targets(target, timeout=2.0), (found,))
            self.assertIs(window.click_target(target, timeout=2.0), found)
            self.assertIs(window.click_best_target(target, timeout=2.0), found)

        screenshot.assert_called_once_with(HWND(123))
        find.assert_called_once_with(
            HWND(123), target, threshold=0.9, timeout=2.0, overlap=0.3
        )
        find_best.assert_called_once_with(
            HWND(123), target, threshold=0.9, timeout=2.0, overlap=0.3
        )
        find_all.assert_called_once_with(
            HWND(123), target, threshold=0.9, timeout=2.0, overlap=0.3
        )
        click.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout=2.0,
            overlap=0.3,
            button="left",
        )
        click_best.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout=2.0,
            overlap=0.3,
            button="left",
        )


if __name__ == "__main__":
    unittest.main()
