from __future__ import annotations

import unittest
from unittest.mock import patch

import numpy as np

from litewinwrap.match import (
    TargetAmbiguousError,
    best_match,
    click,
    find,
    match,
    match_all,
)
from litewinwrap.types import Capture, HWND, Point, Rect, Target


class MatchTests(unittest.TestCase):
    def setUp(self) -> None:
        generator = np.random.default_rng(42)
        self.template = generator.integers(0, 256, (10, 12, 3), dtype=np.uint8)
        self.target = Target("button", self.template, click=(0.25, 0.75))

    def _capture_with_matches(self, *positions: tuple[int, int]) -> Capture:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        for x, y in positions:
            pixels[y : y + 10, x : x + 12] = self.template
        return Capture(pixels, Rect(0, 0, 80, 60))

    def test_returns_absolute_rectangle_and_click_point(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[19:29, 31:43] = self.template
        capture = Capture(pixels, Rect(100, 200, 180, 260))

        found = match(capture, self.target, threshold=0.99)

        self.assertEqual(found.rect, Rect(131, 219, 143, 229))
        self.assertEqual(found.click, Point(134, 226))
        self.assertGreaterEqual(found.score, 0.99)

    def test_keeps_distinct_occurrences(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[5:15, 6:18] = self.template
        pixels[35:45, 50:62] = self.template
        capture = Capture(pixels, Rect(0, 0, 80, 60))

        found = match_all(capture, self.target, threshold=0.99)

        self.assertEqual(len(found), 2)
        self.assertEqual({item.rect.left for item in found}, {6, 50})

    def test_spatially_flat_colour_target_does_not_match_everywhere(self) -> None:
        generator = np.random.default_rng(7)
        pixels = generator.integers(0, 256, (40, 50, 3), dtype=np.uint8)
        template = np.empty((8, 9, 3), dtype=np.uint8)
        template[:] = (10, 80, 170)
        pixels[17:25, 21:30] = template
        capture = Capture(pixels, Rect(0, 0, 50, 40))

        found = match_all(capture, Target("colour", template), threshold=0.99)

        self.assertEqual(tuple(item.rect for item in found), (Rect(21, 17, 30, 25),))

    def test_match_rejects_multiple_occurrences(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[5:15, 6:18] = self.template
        pixels[35:45, 50:62] = self.template
        capture = Capture(pixels, Rect(0, 0, 80, 60))

        with self.assertRaises(TargetAmbiguousError) as raised:
            match(capture, self.target, threshold=0.99)

        self.assertIs(raised.exception.target, self.target)
        self.assertEqual(len(raised.exception.matches), 2)
        self.assertEqual(
            {item.rect.left for item in raised.exception.matches},
            {6, 50},
        )

    def test_best_match_explicitly_selects_highest_score(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[5:15, 6:18] = self.template
        imperfect = self.template.copy()
        imperfect[:2, :2] = 0
        pixels[35:45, 50:62] = imperfect
        capture = Capture(pixels, Rect(0, 0, 80, 60))

        found = best_match(capture, self.target, threshold=0.80)

        self.assertEqual(found.rect, Rect(6, 5, 18, 15))
        self.assertAlmostEqual(found.score, 1.0)

    def test_click_waits_after_successful_input(self) -> None:
        capture = self._capture_with_matches((6, 5))

        with (
            patch("litewinwrap.match.capture", return_value=capture),
            patch("litewinwrap.match.mouse.click") as send_click,
            patch("litewinwrap.match.time.sleep") as sleep,
        ):
            found = click(
                HWND(123),
                self.target,
                threshold=0.99,
                wait_after_seconds=0.2,
            )

        send_click.assert_called_once_with(found.click, button="left")
        sleep.assert_called_once_with(0.2)

    def test_find_can_retry_transient_ambiguity_until_match_is_unique(self) -> None:
        ambiguous = self._capture_with_matches((6, 5), (50, 35))
        unique = self._capture_with_matches((50, 35))

        with (
            patch(
                "litewinwrap.match.capture",
                side_effect=(ambiguous, unique),
            ) as capture_window,
            patch("litewinwrap.match.time.monotonic", side_effect=(0.0, 0.1)),
            patch("litewinwrap.match.time.sleep") as sleep,
        ):
            found = find(
                HWND(123),
                self.target,
                threshold=0.99,
                timeout_seconds=1.0,
                retry_on_ambiguity=True,
            )

        self.assertEqual(found.rect, Rect(50, 35, 62, 45))
        self.assertEqual(capture_window.call_count, 2)
        sleep.assert_called_once_with(0.05)

    def test_find_reports_ambiguity_immediately_by_default(self) -> None:
        ambiguous = self._capture_with_matches((6, 5), (50, 35))

        with (
            patch(
                "litewinwrap.match.capture",
                return_value=ambiguous,
            ) as capture_window,
            patch("litewinwrap.match.time.monotonic", return_value=0.0),
            patch("litewinwrap.match.time.sleep") as sleep,
        ):
            with self.assertRaises(TargetAmbiguousError):
                find(
                    HWND(123),
                    self.target,
                    threshold=0.99,
                    timeout_seconds=1.0,
                )

        capture_window.assert_called_once_with(HWND(123))
        sleep.assert_not_called()

    def test_find_reports_last_ambiguity_at_retry_deadline(self) -> None:
        first = self._capture_with_matches((6, 5), (50, 35))
        last = self._capture_with_matches((10, 10), (40, 30))

        with (
            patch("litewinwrap.match.capture", side_effect=(first, last)),
            patch(
                "litewinwrap.match.time.monotonic",
                side_effect=(0.0, 0.2, 1.0),
            ),
            patch("litewinwrap.match.time.sleep"),
        ):
            with self.assertRaises(TargetAmbiguousError) as raised:
                find(
                    HWND(123),
                    self.target,
                    threshold=0.99,
                    timeout_seconds=1.0,
                    retry_on_ambiguity=True,
                )

        self.assertEqual(
            {item.rect.left for item in raised.exception.matches},
            {10, 40},
        )


if __name__ == "__main__":
    unittest.main()
