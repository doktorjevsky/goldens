from __future__ import annotations

import unittest
from unittest.mock import patch

from litewinwrap import keyboard


class KeyboardTests(unittest.TestCase):
    def test_press_accepts_a_named_key_chord(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=4) as send:
            self.assertEqual(keyboard.press("ctrl", "q"), 4)

        inputs = send.call_args.args[0]
        self.assertEqual([item.ki.wVk for item in inputs], [0x11, 0x51, 0x51, 0x11])
        self.assertEqual(
            [bool(item.ki.dwFlags & keyboard.KEYEVENTF_KEYUP) for item in inputs],
            [False, False, True, True],
        )

    def test_press_accepts_aliases_function_keys_and_numpad_keys(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=6) as send:
            keyboard.press("escape", "f12", "numpad7")

        inputs = send.call_args.args[0]
        self.assertEqual([item.ki.wVk for item in inputs[:3]], [0x1B, 0x7B, 0x67])

    def test_press_rejects_an_unknown_key_name(self) -> None:
        with self.assertRaisesRegex(ValueError, "Unknown keyboard key"):
            keyboard.press("definitely-not-a-key")

    def test_type_text_sends_literal_unicode_units(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=4) as send:
            self.assertEqual(keyboard.type_text("Aé"), 4)

        inputs = send.call_args.args[0]
        self.assertEqual([item.ki.wScan for item in inputs], [ord("A"), ord("A"), 0xE9, 0xE9])
        self.assertTrue(all(item.ki.dwFlags & keyboard.KEYEVENTF_UNICODE for item in inputs))

    def test_down_and_up_accept_named_keys(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=1) as send:
            keyboard.down("shift")
            keyboard.up("shift")

        down = send.call_args_list[0].args[0][0]
        up = send.call_args_list[1].args[0][0]
        self.assertEqual(down.ki.wVk, keyboard.VK_SHIFT)
        self.assertFalse(down.ki.dwFlags & keyboard.KEYEVENTF_KEYUP)
        self.assertTrue(up.ki.dwFlags & keyboard.KEYEVENTF_KEYUP)

    def test_hold_presses_a_chord_and_releases_it_in_reverse_order(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=2) as send:
            with keyboard.hold("ctrl", "shift"):
                self.assertEqual(send.call_count, 1)

        pressed = send.call_args_list[0].args[0]
        released = send.call_args_list[1].args[0]
        self.assertEqual([item.ki.wVk for item in pressed], [0x11, 0x10])
        self.assertEqual([item.ki.wVk for item in released], [0x10, 0x11])
        self.assertTrue(
            all(item.ki.dwFlags & keyboard.KEYEVENTF_KEYUP for item in released)
        )

    def test_hold_releases_keys_when_the_body_raises(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input", return_value=1) as send:
            with self.assertRaisesRegex(RuntimeError, "body failed"):
                with keyboard.hold("ctrl"):
                    raise RuntimeError("body failed")

        self.assertEqual(send.call_count, 2)
        released = send.call_args_list[1].args[0][0]
        self.assertTrue(released.ki.dwFlags & keyboard.KEYEVENTF_KEYUP)

    def test_hold_validates_every_key_before_pressing_any(self) -> None:
        with patch("litewinwrap.keyboard.win32.send_input") as send:
            with self.assertRaisesRegex(ValueError, "Unknown keyboard key"):
                with keyboard.hold("ctrl", "definitely-not-a-key"):
                    pass

        send.assert_not_called()


if __name__ == "__main__":
    unittest.main()
