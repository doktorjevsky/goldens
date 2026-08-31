from __future__ import annotations

import unittest
from unittest.mock import call, patch

from litewinwrap import mouse
from litewinwrap.types import Point


class MouseTests(unittest.TestCase):
    def test_invalid_click_intervals_are_rejected_before_moving_or_clicking(self) -> None:
        invalid = (-0.1, float("inf"), float("nan"))
        with (
            patch("litewinwrap.mouse.move_to") as move_to,
            patch("litewinwrap.mouse.win32.send_input") as send,
        ):
            for interval_seconds in invalid:
                with self.subTest(value=interval_seconds):
                    with self.assertRaisesRegex(ValueError, "interval_seconds"):
                        mouse.click(
                            (10, 20),
                            count=2,
                            interval_seconds=interval_seconds,
                        )

        move_to.assert_not_called()
        send.assert_not_called()

    def test_hold_releases_the_button_when_the_body_raises(self) -> None:
        events: list[tuple[str, str]] = []

        with (
            patch(
                "litewinwrap.mouse.button_down",
                side_effect=lambda button: events.append(("down", button)),
            ),
            patch(
                "litewinwrap.mouse.button_up",
                side_effect=lambda button: events.append(("up", button)),
            ),
        ):
            with self.assertRaisesRegex(RuntimeError, "body failed"):
                with mouse.hold("right"):
                    raise RuntimeError("body failed")

        self.assertEqual(events, [("down", "right"), ("up", "right")])

    def test_drag_to_moves_to_an_optional_origin_then_releases(self) -> None:
        with (
            patch("litewinwrap.mouse.move_to", side_effect=(1, 2)) as move_to,
            patch("litewinwrap.mouse.button_down", return_value=3) as down,
            patch("litewinwrap.mouse.button_up", return_value=4) as up,
        ):
            sent = mouse.drag_to(
                (30, 40),
                origin=(10, 20),
                button="middle",
                duration_seconds=0.4,
            )

        self.assertEqual(sent, 10)
        self.assertEqual(
            move_to.call_args_list,
            [call((10, 20)), call((30, 40), duration_seconds=0.4)],
        )
        down.assert_called_once_with("middle")
        up.assert_called_once_with("middle")

    def test_drag_by_moves_relatively_and_releases(self) -> None:
        with (
            patch("litewinwrap.mouse.move_by", return_value=2) as move_by,
            patch("litewinwrap.mouse.button_down", return_value=1) as down,
            patch("litewinwrap.mouse.button_up", return_value=1) as up,
        ):
            sent = mouse.drag_by(
                100,
                -20,
                button="left",
                duration_seconds=0.4,
            )

        self.assertEqual(sent, 4)
        move_by.assert_called_once_with(100, -20, duration_seconds=0.4)
        down.assert_called_once_with("left")
        up.assert_called_once_with("left")

    def test_drag_uses_a_nonzero_default_duration(self) -> None:
        with (
            patch("litewinwrap.mouse.move_by", return_value=2) as move_by,
            patch("litewinwrap.mouse.button_down", return_value=1),
            patch("litewinwrap.mouse.button_up", return_value=1),
        ):
            mouse.drag_by(100, 0)

        move_by.assert_called_once_with(100, 0, duration_seconds=0.25)

    def test_move_to_interpolates_absolute_positions_over_the_duration(self) -> None:
        with (
            patch(
                "litewinwrap.mouse.win32.get_system_metric",
                side_effect=(0, 0, 1001, 1001),
            ),
            patch(
                "litewinwrap.mouse.win32.get_cursor_position",
                return_value=Point(0, 0),
            ),
            patch(
                "litewinwrap.mouse._absolute_move_input",
                return_value=object(),
            ) as event,
            patch("litewinwrap.mouse.win32.send_input", return_value=1) as send,
            patch("litewinwrap.mouse.time.sleep") as sleep,
        ):
            sent = mouse.move_to((30, 15), duration_seconds=0.05)

        self.assertEqual(sent, 3)
        self.assertEqual(
            [item.args[0] for item in event.call_args_list],
            [Point(30, 15), Point(10, 5), Point(20, 10), Point(30, 15)],
        )
        self.assertEqual(send.call_count, 3)
        self.assertEqual(sleep.call_count, 3)
        for sleep_call in sleep.call_args_list:
            self.assertAlmostEqual(sleep_call.args[0], 1.0 / 60.0)

    def test_instant_move_is_absolute_and_not_coalesced(self) -> None:
        with (
            patch(
                "litewinwrap.mouse.win32.get_system_metric",
                side_effect=(0, 0, 1001, 1001),
            ),
            patch("litewinwrap.mouse.win32.send_input", return_value=1) as send,
        ):
            mouse.move_to((500, 250))

        event = send.call_args.args[0][0]
        self.assertEqual(event.mi.dx, round(500 * 65535 / 1000))
        self.assertEqual(event.mi.dy, round(250 * 65535 / 1000))
        self.assertTrue(event.mi.dwFlags & mouse.MOUSEEVENTF_ABSOLUTE)
        self.assertTrue(event.mi.dwFlags & mouse.MOUSEEVENTF_VIRTUALDESK)
        self.assertTrue(event.mi.dwFlags & mouse.MOUSEEVENTF_MOVE_NOCOALESCE)

    def test_move_by_uses_an_absolute_destination_to_avoid_acceleration(self) -> None:
        with (
            patch(
                "litewinwrap.mouse.win32.get_cursor_position",
                return_value=Point(100, 200),
            ),
            patch("litewinwrap.mouse.move_to", return_value=1) as move_to,
        ):
            sent = mouse.move_by(30, -20, duration_seconds=0.2)

        self.assertEqual(sent, 1)
        move_to.assert_called_once_with(Point(130, 180), duration_seconds=0.2)

    def test_negative_duration_is_rejected_before_a_drag_starts(self) -> None:
        with patch("litewinwrap.mouse.button_down") as down:
            with self.assertRaisesRegex(ValueError, "Movement duration"):
                mouse.drag_by(1, 2, duration_seconds=-0.1)

        down.assert_not_called()

    def test_drag_by_releases_the_button_when_movement_raises(self) -> None:
        with (
            patch("litewinwrap.mouse.button_down", return_value=1),
            patch("litewinwrap.mouse.move_by", side_effect=RuntimeError("move failed")),
            patch("litewinwrap.mouse.button_up", return_value=1) as up,
        ):
            with self.assertRaisesRegex(RuntimeError, "move failed"):
                mouse.drag_by(1, 2)

        up.assert_called_once_with("left")


if __name__ == "__main__":
    unittest.main()
