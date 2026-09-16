from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

from litewinwrap import (
    Goldens,
    GoldensFormatError,
    InconsistentGoldenScaleError,
)


class GoldensTests(unittest.TestCase):
    @staticmethod
    def _write_resource(
        png: Path,
        annotations: list[dict[str, object]],
        *,
        shape: tuple[int, int] = (20, 30),
        scale: float = 1.0,
    ) -> None:
        png.parent.mkdir(parents=True, exist_ok=True)
        height, width = shape
        image = np.zeros((height, width, 3), dtype=np.uint8)
        image[4:10, 8:18] = (10, 20, 30)
        if not cv2.imwrite(str(png), image):
            raise AssertionError(f"Could not write test image: {png}")
        png.with_suffix(".json").write_text(
            json.dumps({"scale": scale, "annotations": annotations}),
            encoding="utf-8",
        )

    def test_exposes_annotation_crops_as_a_mapping(self) -> None:
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
            self.assertEqual(goldens.scale, 1.0)
            self.assertEqual(
                tuple(goldens),
                ("button_0", "button_1"),
            )
            self.assertEqual(len(goldens), 2)
            self.assertIn("button_0", goldens)
            target = goldens["button_0"]
            self.assertEqual(target.name, "button_0")
            self.assertEqual(target.pixels.shape, (6, 10, 3))
            self.assertEqual(target.click, (0.25, 0.75))
            self.assertIsNone(goldens["button_1"].click)
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
            self.assertEqual(goldens.scale, 1.0)
            self.assertEqual(goldens.paths, (calculator, login))
            self.assertEqual(
                tuple(goldens),
                ("submit", "dialogs/submit"),
            )
            self.assertEqual(
                goldens["dialogs/submit"].name,
                "dialogs/submit",
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

    def test_rejects_inconsistent_scales_below_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boundary = {"x": 0, "y": 0, "width": 1, "height": 1}
            first = root / "first.png"
            second = root / "second.png"
            self._write_resource(
                first,
                [{"name": "first", "boundary": boundary}],
                scale=1.0,
            )
            self._write_resource(
                second,
                [{"name": "second", "boundary": boundary}],
                scale=1.25,
            )

            with self.assertRaises(InconsistentGoldenScaleError) as raised:
                Goldens.from_root(root)

            self.assertEqual(raised.exception.expected_path, first)
            self.assertEqual(raised.exception.expected_scale, 1.0)
            self.assertEqual(raised.exception.conflicting_path, second)
            self.assertEqual(raised.exception.conflicting_scale, 1.25)

    def test_tree_requires_an_explicit_scale_for_every_resource(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            png = root / "legacy.png"
            self._write_resource(png, [])
            png.with_suffix(".json").write_text(
                json.dumps({"annotations": []}),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(GoldensFormatError, "no capture scale"):
                Goldens.from_root(root)

            legacy = Goldens.from_png(png)
            self.assertIsNone(legacy.scale)

    def test_rejects_invalid_scale_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "invalid-scale.png"
            self._write_resource(png, [])

            for value in (None, False, 0, -1, 10**1000):
                with self.subTest(value=value):
                    png.with_suffix(".json").write_text(
                        json.dumps({"scale": value, "annotations": []}),
                        encoding="utf-8",
                    )
                    with self.assertRaisesRegex(
                        GoldensFormatError,
                        "finite positive number",
                    ):
                        Goldens.from_png(png)

    def test_rejects_nonstandard_or_ambiguous_json(self) -> None:
        invalid_documents = {
            "duplicate key": (
                '{"annotations":[{"name":"first","name":"second",'
                '"boundary":{"x":0,"y":0,"width":1,"height":1}}]}'
            ),
            "non-finite number": '{"annotations":[],"unknown":NaN}',
            "unpaired surrogate": (
                '{"annotations":[{"name":"\\ud800",'
                '"boundary":{"x":0,"y":0,"width":1,"height":1}}]}'
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "invalid.png"
            self._write_resource(png, [])

            for name, document in invalid_documents.items():
                with self.subTest(name=name):
                    png.with_suffix(".json").write_text(document, encoding="utf-8")
                    with self.assertRaisesRegex(
                        GoldensFormatError,
                        "golden sidecar",
                    ):
                        Goldens.from_png(png)

    def test_accepts_valid_supplementary_unicode_in_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            png = Path(directory) / "unicode.png"
            self._write_resource(
                png,
                [
                    {
                        "name": "button_😀",
                        "boundary": {
                            "x": 0,
                            "y": 0,
                            "width": 1,
                            "height": 1,
                        },
                    }
                ],
            )

            goldens = Goldens.from_png(png)

            self.assertIn("button_😀", goldens)

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

            self._write_resource(
                png,
                [
                    {
                        "name": "dialog\\submit",
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

    def test_png_names_do_not_contribute_to_folder_identifiers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boundary = {"x": 0, "y": 0, "width": 1, "height": 1}
            self._write_resource(
                root / "homepage" / "wide.png",
                [{"name": "hero", "boundary": boundary}],
            )
            self._write_resource(
                root / "homepage" / "narrow.png",
                [{"name": "nav", "boundary": boundary}],
            )
            self._write_resource(
                root / "homepage" / "account" / "dialog.png",
                [{"name": "submit", "boundary": boundary}],
            )

            goldens = Goldens.from_root(root)

            self.assertEqual(
                set(goldens),
                {"homepage/hero", "homepage/nav", "homepage/account/submit"},
            )

    def test_rejects_duplicate_annotations_across_pngs_in_one_folder(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            boundary = {"x": 0, "y": 0, "width": 1, "height": 1}
            self._write_resource(
                root / "homepage" / "wide.png",
                [{"name": "hero", "boundary": boundary}],
            )
            self._write_resource(
                root / "homepage" / "narrow.png",
                [{"name": "HERO", "boundary": boundary}],
            )

            with self.assertRaisesRegex(
                GoldensFormatError,
                "Duplicate target identifier",
            ):
                Goldens.from_root(root)

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
