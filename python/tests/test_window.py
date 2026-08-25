from __future__ import annotations

import unittest
from unittest.mock import patch

from litewinwrap.types import HWND
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


if __name__ == "__main__":
    unittest.main()
