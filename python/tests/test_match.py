from __future__ import annotations

import unittest

import numpy as np

from litewinwrap.match import match
from litewinwrap.types import Capture, Point, Rect, Target


class MatchTests(unittest.TestCase):
    def setUp(self) -> None:
        generator = np.random.default_rng(42)
        self.template = generator.integers(0, 256, (10, 12, 3), dtype=np.uint8)
        self.target = Target("button", self.template, click=(0.25, 0.75))

    def test_returns_absolute_rectangle_and_click_point(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[19:29, 31:43] = self.template
        capture = Capture(pixels, Rect(100, 200, 180, 260))

        found = match(capture, self.target, threshold=0.99)

        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].rect, Rect(131, 219, 143, 229))
        self.assertEqual(found[0].click, Point(134, 226))
        self.assertGreaterEqual(found[0].score, 0.99)

    def test_keeps_distinct_occurrences(self) -> None:
        pixels = np.zeros((60, 80, 3), dtype=np.uint8)
        pixels[5:15, 6:18] = self.template
        pixels[35:45, 50:62] = self.template
        capture = Capture(pixels, Rect(0, 0, 80, 60))

        found = match(capture, self.target, threshold=0.99)

        self.assertEqual(len(found), 2)
        self.assertEqual({item.rect.left for item in found}, {6, 50})


if __name__ == "__main__":
    unittest.main()
