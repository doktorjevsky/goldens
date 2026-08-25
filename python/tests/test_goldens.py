from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from litewinwrap import Goldens, GoldensFormatError


class GoldensTests(unittest.TestCase):
    def test_exposes_annotation_crops_as_a_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "calculator.png"
            image = np.zeros((20, 30, 3), dtype=np.uint8)
            image[4:10, 8:18] = (10, 20, 30)
            self.assertTrue(cv2.imwrite(str(png), image))
            png.with_suffix(".json").write_text(
                json.dumps(
                    {
                        "annotations": [
                            {
                                "name": "button_0",
                                "boundary": {
                                    "x": 8,
                                    "y": 4,
                                    "width": 10,
                                    "height": 6,
                                },
                                "click": {"x": 0.25, "y": 0.75},
                            },
                            {
                                "name": "button_1",
                                "boundary": {
                                    "x": 0,
                                    "y": 0,
                                    "width": 4,
                                    "height": 3,
                                },
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )

            goldens = Goldens(png)

            self.assertEqual(goldens.path, png)
            self.assertEqual(tuple(goldens), ("button_0", "button_1"))
            self.assertEqual(len(goldens), 2)
            self.assertIn("button_0", goldens)
            self.assertEqual(goldens["button_0"].pixels.shape, (6, 10, 3))
            self.assertEqual(goldens["button_0"].click, (0.25, 0.75))
            self.assertIsNone(goldens["button_1"].click)
            self.assertFalse(goldens["button_0"].pixels.flags.writeable)
            self.assertTrue(goldens["button_0"].pixels.flags.owndata)

    def test_rejects_duplicate_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "duplicate.png"
            self.assertTrue(cv2.imwrite(str(png), np.zeros((5, 5, 3), np.uint8)))
            annotation = {
                "name": "same",
                "boundary": {"x": 0, "y": 0, "width": 1, "height": 1},
            }
            png.with_suffix(".json").write_text(
                json.dumps({"annotations": [annotation, annotation]}),
                encoding="utf-8",
            )

            with self.assertRaises(GoldensFormatError):
                Goldens(png)


if __name__ == "__main__":
    unittest.main()
