"""Opt-in integration check against an external native test application."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

from litewinwrap import (
    Automation,
    ElementNotFoundError,
    ElementReadOnlyError,
    ElementUnsupportedError,
)


@unittest.skipUnless(
    sys.platform == "win32" and os.environ.get("LITEWINWRAP_UI_TEST_APP"),
    "Set LITEWINWRAP_UI_TEST_APP to the external WindowsUITest.exe",
)
class LiveUIElementTests(unittest.TestCase):
    def test_fields_checkboxes_buttons_and_debug_listing(self):
        process = subprocess.Popen([os.environ["LITEWINWRAP_UI_TEST_APP"]])
        try:
            automation = Automation(timeout_seconds=5, settle_seconds=0.05)
            window = automation.find_window(
                class_name="WindowsUIAutomationTestApp",
                process_id=process.pid,
                visible_only=False,
            )
            listing = window.elements(visible_only=False)
            ids = {element.automation_id for element in listing}
            self.assertTrue({str(i) for i in range(1001, 1008)} <= ids)

            def find(id):
                return window.find_element(automation_id=str(id), visible_only=False)

            name, folder, logging, dry_run, run, ok, results = (
                find(i) for i in range(1001, 1008)
            )
            self.assertIn("Value", name.patterns)
            self.assertIn("Toggle", logging.patterns)
            self.assertIn("Invoke", run.patterns)
            name.set_value("Alice \u00c5 \U0001f44b")
            folder.set_value("C:\\Temp\\Automation")
            self.assertEqual(name.value, "Alice \u00c5 \U0001f44b")
            self.assertTrue(logging.checked)
            self.assertFalse(dry_run.checked)
            logging.set_checked(False).set_checked(False)
            dry_run.set_checked(True).set_checked(True)
            run.invoke()
            output = results.value
            for expected in (
                "Action: Run",
                "Submission: 1",
                "Name: Alice \u00c5 \U0001f44b",
                "Output folder: C:\\Temp\\Automation",
                "Enable logging: Unchecked",
                "Dry run: Checked",
            ):
                self.assertIn(expected, output)
            logging.toggle()
            dry_run.set_checked(False)
            ok.invoke()
            for expected in (
                "Action: OK",
                "Submission: 2",
                "Enable logging: Checked",
                "Dry run: Unchecked",
            ):
                self.assertIn(expected, results.value)
            with self.assertRaises(ElementReadOnlyError):
                results.set_value("cannot change this")
            with self.assertRaises(ElementUnsupportedError):
                name.invoke()
            with self.assertRaises(ElementNotFoundError):
                window.find_element(automation_id="missing", timeout_seconds=0)
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
