from __future__ import annotations

import unittest
from unittest.mock import patch

import numpy as np

from litewinwrap import Automation
from litewinwrap.types import Capture, HWND, Match, Point, Rect, Target
from litewinwrap.window import Window


class WindowTests(unittest.TestCase):
    def test_automation_policy_is_used_when_options_are_omitted(self) -> None:
        automation = Automation(
            timeout_seconds=2.5,
            settle_seconds=0.3,
            threshold=0.85,
            overlap=0.2,
            retry_on_ambiguity=True,
        )
        window = automation.window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with patch("litewinwrap.match.find", return_value=found) as find:
            self.assertIs(window.locate(target), found)

        find.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.85,
            timeout_seconds=2.5,
            overlap=0.2,
            retry_on_ambiguity=True,
        )

    def test_explicit_zero_timeout_seconds_overrides_default(self) -> None:
        window = Automation(timeout_seconds=2.5).window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with patch("litewinwrap.match.find", return_value=found) as find:
            self.assertIs(window.locate(target, timeout_seconds=0.0), found)

        self.assertEqual(find.call_args.kwargs["timeout_seconds"], 0.0)

    def test_session_settle_seconds_is_used_by_click(self) -> None:
        window = Automation(
            settle_seconds=0.3,
            focus_before_input=False,
        ).window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with patch("litewinwrap.match.click", return_value=found) as click:
            self.assertIs(window.click(target), found)

        self.assertEqual(click.call_args.kwargs["wait_after_seconds"], 0.3)

    def test_hover_locates_moves_and_uses_the_session_settle_seconds(self) -> None:
        window = Automation(
            settle_seconds=0.3,
            focus_before_input=False,
        ).window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with (
            patch("litewinwrap.window.Window.locate", return_value=found) as locate,
            patch("litewinwrap.window.mouse.move_to", return_value=1) as move_to,
            patch("litewinwrap.automation.time.sleep") as sleep,
        ):
            self.assertIs(window.hover(target), found)

        locate.assert_called_once_with(
            target,
            threshold=None,
            timeout_seconds=None,
            overlap=None,
            retry_on_ambiguity=None,
        )
        move_to.assert_called_once_with(found.click)
        sleep.assert_called_once_with(0.3)

    def test_hover_can_override_focus_and_settling_per_call(self) -> None:
        window = Automation(
            settle_seconds=0.3,
            focus_before_input=True,
        ).window(HWND(123))
        target = Target("button", np.zeros((2, 2, 3), dtype=np.uint8))
        found = Match("button", 0.99, Rect(2, 3, 4, 5), Point(3, 4))

        with (
            patch("litewinwrap.window.Window.focus") as focus,
            patch("litewinwrap.window.Window.locate", return_value=found),
            patch("litewinwrap.window.mouse.move_to", return_value=1),
            patch("litewinwrap.automation.time.sleep") as sleep,
        ):
            window.hover(target, focus=False, settle_seconds=0.0)

        focus.assert_not_called()
        sleep.assert_not_called()

    def test_title_is_queried_on_every_access(self) -> None:
        window = Automation(focus_before_input=False).window(HWND(123))
        with patch(
            "litewinwrap.window.win32.get_window_text",
            side_effect=["first", "second"],
        ) as get_window_text:
            self.assertEqual(window.title, "first")
            self.assertEqual(window.title, "second")

        self.assertEqual(get_window_text.call_count, 2)
        self.assertFalse(hasattr(window, "__dict__"))

    def test_direct_children_are_filtered_from_descendants(self) -> None:
        window = Automation().window(HWND(100))
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
            children = window.children(recursive=False, visible_only=False)

        self.assertEqual(tuple(int(child.hwnd) for child in children), (101, 102))
        self.assertTrue(all(child.automation is window.automation for child in children))

    def test_primary_visual_methods_delegate_with_session_policy(self) -> None:
        window = Automation(focus_before_input=False).window(HWND(123))
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
            self.assertIs(window.capture(), capture)
            self.assertIs(
                window.locate(
                    target,
                    timeout_seconds=2.0,
                    retry_on_ambiguity=True,
                ),
                found,
            )
            self.assertIs(window.locate_best(target, timeout_seconds=2.0), found)
            self.assertEqual(window.locate_all(target, timeout_seconds=2.0), (found,))
            self.assertIs(
                window.click(
                    target,
                    timeout_seconds=2.0,
                    retry_on_ambiguity=True,
                ),
                found,
            )
            self.assertIs(window.click_best(target, timeout_seconds=2.0), found)

        screenshot.assert_called_once_with(HWND(123))
        find.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout_seconds=2.0,
            overlap=0.3,
            retry_on_ambiguity=True,
        )
        find_best.assert_called_once_with(
            HWND(123), target, threshold=0.9, timeout_seconds=2.0, overlap=0.3
        )
        find_all.assert_called_once_with(
            HWND(123), target, threshold=0.9, timeout_seconds=2.0, overlap=0.3
        )
        click.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout_seconds=2.0,
            overlap=0.3,
            retry_on_ambiguity=True,
            button="left",
            wait_after_seconds=0.15,
        )
        click_best.assert_called_once_with(
            HWND(123),
            target,
            threshold=0.9,
            timeout_seconds=2.0,
            overlap=0.3,
            button="left",
            wait_after_seconds=0.15,
        )

    def test_text_and_keys_focus_the_window_and_settle(self) -> None:
        window = Automation(settle_seconds=0.2).window(HWND(123))

        with (
            patch("litewinwrap.window.win32.get_foreground_window", return_value=HWND(999)),
            patch("litewinwrap.window.Window.focus", return_value=window) as focus,
            patch("litewinwrap.window.keyboard.type_text", return_value=2) as type_text,
            patch("litewinwrap.window.keyboard.press", return_value=4) as press,
            patch("litewinwrap.automation.time.sleep") as sleep,
        ):
            self.assertIs(window.type_text("hi"), window)
            self.assertIs(window.press("ctrl", "q"), window)

        type_text.assert_called_once_with(
            "hi", interval_seconds=0.0, chunk_size=256
        )
        press.assert_called_once_with(
            "ctrl", "q", count=1, interval_seconds=0.0
        )
        self.assertEqual(focus.call_count, 2)
        focus.assert_called_with(settle_seconds=0.0)
        self.assertEqual(sleep.call_args_list[0].args, (0.2,))
        self.assertEqual(sleep.call_args_list[1].args, (0.2,))

    def test_input_can_override_automatic_focus_per_call(self) -> None:
        window = Automation(focus_before_input=True).window(HWND(123))

        with (
            patch(
                "litewinwrap.window.win32.get_foreground_window",
                return_value=HWND(999),
            ),
            patch("litewinwrap.window.Window.focus") as focus,
            patch("litewinwrap.window.keyboard.press", return_value=2),
        ):
            window.press("enter", focus=False, settle_seconds=0.0)

        focus.assert_not_called()


if __name__ == "__main__":
    unittest.main()
