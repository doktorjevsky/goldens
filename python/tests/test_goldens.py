from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from litewinwrap import Goldens, GoldensFormatError


class GoldensTests(unittest.TestCase):
    @staticmethod
    def _write_resource(
        png: Path,
        annotations: list[dict[str, object]],
        *,
        shape: tuple[int, int] = (20, 30),
    ) -> None:
        png.parent.mkdir(parents=True, exist_ok=True)
        height, width = shape
        image = np.zeros((height, width, 3), dtype=np.uint8)
        image[4:10, 8:18] = (10, 20, 30)
        if not cv2.imwrite(str(png), image):
            raise AssertionError(f"Could not write test image: {png}")
        png.with_suffix(".json").write_text(
            json.dumps({"annotations": annotations}),
            encoding="utf-8",
        )

    def test_exposes_qualified_annotation_crops_as_a_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "calculator.png"
            self._write_resource(
                png,
                [
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
                ],
            )

            goldens = Goldens.from_png(png)

            self.assertEqual(goldens.paths, (png,))
            self.assertIsNone(goldens.root)
            self.assertEqual(
                tuple(goldens),
                ("calculator/button_0", "calculator/button_1"),
            )
            self.assertEqual(len(goldens), 2)
            self.assertIn("calculator/button_0", goldens)
            target = goldens["calculator/button_0"]
            self.assertEqual(target.name, "calculator/button_0")
            self.assertEqual(target.pixels.shape, (6, 10, 3))
            self.assertEqual(target.click, (0.25, 0.75))
            self.assertIsNone(goldens["calculator/button_1"].click)
            self.assertFalse(target.pixels.flags.writeable)
            self.assertTrue(target.pixels.flags.owndata)

    def test_loads_annotated_pngs_below_root_recursively(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calculator = root / "calculator.png"
            login = root / "dialogs" / "login.png"
            boundary = {"x": 0, "y": 0, "width": 2, "height": 2}
            self._write_resource(
                calculator,
                [{"name": "submit", "boundary": boundary}],
            )
            self._write_resource(
                login,
                [{"name": "submit", "boundary": boundary}],
            )

            unannotated = root / "unannotated.png"
            self.assertTrue(
                cv2.imwrite(str(unannotated), np.zeros((2, 2, 3), np.uint8))
            )

            goldens = Goldens.from_root(root)

            self.assertEqual(goldens.root, root)
            self.assertEqual(goldens.paths, (calculator, login))
            self.assertEqual(
                tuple(goldens),
                ("calculator/submit", "dialogs/login/submit"),
            )
            self.assertEqual(
                goldens["dialogs/login/submit"].name,
                "dialogs/login/submit",
            )

    def test_rejects_case_insensitive_identifier_collisions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "duplicate.png"
            boundary = {"x": 0, "y": 0, "width": 1, "height": 1}
            self._write_resource(
                png,
                [
                    {"name": "same", "boundary": boundary},
                    {"name": "SAME", "boundary": boundary},
                ],
            )

            with self.assertRaisesRegex(
                GoldensFormatError,
                "Duplicate target identifier",
            ):
                Goldens.from_png(png)

    def test_rejects_namespace_separator_in_annotation_name(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "invalid.png"
            self._write_resource(
                png,
                [
                    {
                        "name": "dialog/submit",
                        "boundary": {
                            "x": 0,
                            "y": 0,
                            "width": 1,
                            "height": 1,
                        },
                    }
                ],
            )

            with self.assertRaisesRegex(GoldensFormatError, "reserved"):
                Goldens.from_png(png)

    def test_requires_an_explicit_loading_mode(self) -> None:
        with self.assertRaisesRegex(TypeError, "from_png.*from_root"):
            Goldens()

    def test_rejects_a_non_directory_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "missing"
            with self.assertRaisesRegex(GoldensFormatError, "not a directory"):
                Goldens.from_root(missing)


if __name__ == "__main__":
    unittest.main()
