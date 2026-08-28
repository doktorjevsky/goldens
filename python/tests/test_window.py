from __future__ import annotations

import unittest
from unittest.mock import patch

import numpy as np

from litewinwrap.types import Capture, HWND, Match, Point, Rect, Target
from litewinwrap.window import Window


class WindowTests(unittest.TestCase):
    def test_default_timeout_is_used_when_search_timeout_is_omitted(self) -> None:
        window = Window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))
        previous = Window.default_timeout

        try:
            Window.set_default_timeout(2.5)
            with patch("litewinwrap.match.find", return_value=found) as find:
                self.assertIs(window.find_target(target), found)

            find.assert_called_once_with(
                HWND(123),
                target,
                threshold=0.9,
                timeout=2.5,
                overlap=0.3,
                retry_on_ambiguity=False,
            )
        finally:
            Window.set_default_timeout(previous)

    def test_explicit_zero_timeout_overrides_default(self) -> None:
        window = Window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))
        previous = Window.default_timeout

        try:
            Window.set_default_timeout(2.5)
            with patch("litewinwrap.match.find", return_value=found) as find:
                self.assertIs(window.find_target(target, timeout=0.0), found)

            self.assertEqual(find.call_args.kwargs["timeout"], 0.0)
        finally:
            Window.set_default_timeout(previous)

    def test_default_timeout_must_be_finite_and_non_negative(self) -> None:
        for timeout in (-1.0, float("inf"), float("nan")):
            with self.subTest(timeout=timeout):
                with self.assertRaises(ValueError):
                    Window.set_default_timeout(timeout)

    def test_default_post_click_delay_is_configurable(self) -> None:
        window = Window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))
        previous = Window.default_post_click_delay

        try:
            Window.set_default_post_click_delay(0.3)
            with patch("litewinwrap.match.click", return_value=found) as click:
                self.assertIs(window.click_target(target), found)

            self.assertEqual(click.call_args.kwargs["wait_after"], 0.3)
        finally:
            Window.set_default_post_click_delay(previous)

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
            self.assertIs(
                window.find_target(
                    target,
                    timeout=2.0,
                    retry_on_ambiguity=True,
                ),
                found,
            )
            self.assertIs(window.find_best_target(target, timeout=2.0), found)
            self.assertEqual(window.find_targets(target, timeout=2.0), (found,))
            self.assertIs(
                window.click_target(
                    target,
                    timeout=2.0,
                    retry_on_ambiguity=True,
                ),
                found,
            )
            self.assertIs(window.click_best_target(target, timeout=2.0), found)

        screenshot.assert_called_once_with(HWND(123))
        find.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout=2.0,
            overlap=0.3,
            retry_on_ambiguity=True,
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
            retry_on_ambiguity=True,
            button="left",
            wait_after=0.15,
        )
        click_best.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout=2.0,
            overlap=0.3,
            button="left",
            wait_after=0.15,
        )


if __name__ == "__main__":
    unittest.main()
