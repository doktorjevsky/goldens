from __future__ import annotations

import unittest
from unittest.mock import call, patch

from litewinwrap import mouse


class MouseTests(unittest.TestCase):
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
            sent = mouse.drag_to((30, 40), origin=(10, 20), button="middle")

        self.assertEqual(sent, 10)
        self.assertEqual(move_to.call_args_list, [call((10, 20)), call((30, 40))])
        down.assert_called_once_with("middle")
        up.assert_called_once_with("middle")

    def test_drag_by_moves_relatively_and_releases(self) -> None:
        with (
            patch("litewinwrap.mouse.move_by", return_value=2) as move_by,
            patch("litewinwrap.mouse.button_down", return_value=1) as down,
            patch("litewinwrap.mouse.button_up", return_value=1) as up,
        ):
            sent = mouse.drag_by(100, -20, button="left")

        self.assertEqual(sent, 4)
        move_by.assert_called_once_with(100, -20)
        down.assert_called_once_with("left")
        up.assert_called_once_with("left")

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
