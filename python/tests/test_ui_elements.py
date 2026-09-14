from __future__ import annotations

import re
import json
import tempfile
import unittest
from unittest.mock import patch

from litewinwrap import (
    Automation,
    Element,
    ElementAmbiguousError,
    ElementNotFoundError,
    ElementReadOnlyError,
    ElementTimeoutError,
    ElementUnavailableError,
    ElementUnsupportedError,
    UIAutomationError,
)
from litewinwrap import _uia


class NativeStub:
    def __init__(
        self,
        name="Run",
        automation_id="1005",
        control_type="Button",
        *,
        offscreen=False,
        enabled=True,
        states=(0, 1),
    ):
        self.values = dict(
            name=name,
            automation_id=automation_id,
            control_type=control_type,
            class_name="Button",
            framework_id="Win32",
            process_id=42,
            hwnd=123,
            enabled=enabled,
            offscreen=offscreen,
            is_password=False,
            rect=(1, 2, 30, 40),
            value="",
            read_only=False,
            toggle_state=states[0],
        )
        self.states = states
        self.actions = []
        self.descendants = ()

    def property(self, name):
        return self.values[name]

    def info(self):
        return {
            key: value
            for key, value in self.values.items()
            if key not in ("value", "read_only", "toggle_state")
        } | {"patterns": ("Invoke", "Value", "Toggle")}

    def elements(self, recursive):
        return self.descendants

    def action(self, name, value=None):
        self.actions.append((name, value))
        if name == "set_value":
            if self.values["read_only"]:
                raise ElementReadOnlyError("read-only")
            self.values["value"] = value
        elif name == "toggle":
            i = self.states.index(self.values["toggle_state"])
            self.values["toggle_state"] = self.states[(i + 1) % len(self.states)]


class ElementTests(unittest.TestCase):
    def setUp(self):
        self.window = Automation(
            timeout_seconds=0, settle_seconds=0, dpi_awareness="unchanged"
        ).window(99)
        self.native = NativeStub()
        self.element = Element(self.window, self.native)

    def test_actions_are_chainable_and_field_value_is_live(self):
        self.assertIs(self.element.set_value("Alice \u00c5 \U0001f44b"), self.element)
        self.assertEqual(self.element.value, "Alice \u00c5 \U0001f44b")
        self.assertIs(self.element.invoke(), self.element)
        self.assertIs(self.element.toggle(), self.element)
        self.assertTrue(self.element.checked)

    def test_set_checked_is_idempotent_in_both_directions(self):
        self.element.set_checked(False)
        self.assertEqual(self.native.actions, [])
        self.assertIs(self.element.set_checked(), self.element)
        self.element.set_checked(True)
        self.assertEqual(self.native.actions, [("toggle", None)])
        self.element.set_checked(False)
        self.element.set_checked(False)
        self.assertEqual(len(self.native.actions), 2)
        self.assertFalse(self.element.checked)

    def test_three_state_checkbox_reaches_requested_boolean(self):
        self.native.states = (0, 1, 2)
        self.native.values["toggle_state"] = 2
        self.assertIsNone(self.element.checked)
        self.element.set_checked(True)
        self.assertEqual(len(self.native.actions), 2)
        self.assertTrue(self.element.checked)
        self.element.set_checked(False)
        self.assertEqual(len(self.native.actions), 4)
        self.assertFalse(self.element.checked)

    def test_disabled_control_times_out_without_action(self):
        self.native.values["enabled"] = False
        with self.assertRaises(ElementTimeoutError):
            self.element.invoke()
        self.assertEqual(self.native.actions, [])

    def test_enabled_wait_and_settle_inherit_policy(self):
        self.window = Automation(
            timeout_seconds=1, settle_seconds=0.3, dpi_awareness="unchanged"
        ).window(99)
        element = Element(self.window, self.native)

        def enable(_seconds):
            self.native.values["enabled"] = True

        self.native.values["enabled"] = False
        with patch("litewinwrap.elements.time.sleep", side_effect=enable) as sleep:
            element.invoke()
        self.assertEqual(sleep.call_args_list[0].args, (0.05,))
        self.assertEqual(sleep.call_args_list[-1].args, (0.3,))
        self.assertEqual(self.native.actions, [("invoke", None)])

    def test_invalid_action_options_do_not_modify_control(self):
        for kwargs in ({"timeout_seconds": -1}, {"settle_seconds": -1}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.element.invoke(**kwargs)
        with self.assertRaises(TypeError):
            self.element.set_checked(1)
        with self.assertRaises(TypeError):
            self.element.set_value(123)
        with self.assertRaises(ValueError):
            self.element.set_value("a\0b")
        self.assertEqual(self.native.actions, [])

    def test_read_only_and_unsupported_actions_propagate(self):
        self.native.values["read_only"] = True
        with self.assertRaises(ElementReadOnlyError):
            self.element.set_value("new")
        self.assertEqual(self.element.value, "")
        with patch.object(
            self.native, "action", side_effect=ElementUnsupportedError("Invoke")
        ):
            with self.assertRaises(ElementUnsupportedError):
                self.element.invoke()

    def test_unchanged_checkbox_reports_timeout(self):
        with patch.object(self.native, "action"):
            with self.assertRaises(ElementTimeoutError):
                self.element.set_checked(True)

    def test_info_and_repr_include_diagnostics_but_omit_field_values(self):
        self.native.values["value"] = "secret value"
        info = self.element.info()
        self.assertEqual(info.automation_id, "1005")
        self.assertEqual(info.rect.width, 29)
        self.assertEqual(info.patterns, ("Invoke", "Value", "Toggle"))
        self.assertFalse(hasattr(info, "value"))
        self.assertNotIn("secret value", repr(self.element))
        self.assertIn("automation_id='1005'", repr(self.element))
        self.native.values["name"] = "OK"
        self.assertEqual(self.element.name, "OK")
        self.assertEqual(info.name, "Run")

    def test_field_actions_are_traced_and_text_is_redacted(self):
        from litewinwrap import Reports

        secret = "do-not-record-this-field-value"
        with tempfile.TemporaryDirectory() as directory:
            reports = Reports(directory)

            @reports.test(title="UI element actions", capture_frames=False)
            def failing_test():
                self.element.set_value(secret)
                self.element.invoke()
                raise RuntimeError("expected failure")

            with self.assertRaises(RuntimeError):
                failing_test()
            trace_path = reports.last_report.parent / "trace.json"
            raw_trace = trace_path.read_text(encoding="utf-8")
            self.assertNotIn(secret, raw_trace)
            trace = json.loads(raw_trace)
            self.assertEqual(
                [a["action"] for a in trace["actions"]],
                ["Set element value", "Invoke element"],
            )
            self.assertEqual(trace["actions"][0]["details"]["characters"], len(secret))

    def test_reporting_uses_window_handle_for_virtual_controls(self):
        self.native.values["hwnd"] = 0
        self.assertIsNone(self.element.native_hwnd)
        self.assertEqual(self.element.hwnd, 99)

    def test_stale_element_has_readable_repr_and_live_error(self):
        with patch.object(
            self.native, "info", side_effect=ElementUnavailableError("read", 0x80040201)
        ):
            self.assertEqual(repr(self.element), "Element(unavailable=True)")
        with patch.object(
            self.native,
            "property",
            side_effect=ElementUnavailableError("read", 0x80040201),
        ):
            with self.assertRaises(ElementUnavailableError):
                _ = self.element.name


class ElementDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.window = Automation(
            timeout_seconds=0, settle_seconds=0, dpi_awareness="unchanged"
        ).window(99)
        self.root = NativeStub()
        self.run = NativeStub()
        self.ok = NativeStub("OK", "1006")
        self.hidden = NativeStub("Hidden", "1008", offscreen=True)
        self.root.descendants = (self.run, self.ok, self.hidden)
        self.binding = patch(
            "litewinwrap.elements._uia.from_handle", return_value=self.root
        )
        self.binding.start()
        self.addCleanup(self.binding.stop)

    def test_listing_and_child_listing_keep_window_policy(self):
        controls = self.window.elements()
        self.assertEqual(tuple(e.name for e in controls), ("Run", "OK"))
        self.assertTrue(all(e.window is self.window for e in controls))
        self.assertEqual(len(self.window.elements(visible_only=False)), 3)
        self.assertEqual(
            len(Element(self.window, self.root).children(visible_only=False)), 3
        )

    def test_find_combines_exact_and_regex_selectors(self):
        self.assertEqual(
            self.window.find_element("Run", control_type="Button").automation_id, "1005"
        )
        self.assertEqual(self.window.find_element(automation_id="1006").name, "OK")
        self.assertEqual(
            len(self.window.find_elements(re.compile("Run|OK"), class_name="Button")), 2
        )
        self.assertEqual(self.window.find_elements("missing"), ())
        with self.assertRaises(ValueError):
            self.window.find_elements(control_type="Buton")

    def test_missing_and_ambiguous_controls_have_explicit_errors(self):
        with self.assertRaisesRegex(ElementNotFoundError, "name='missing'.*0.000s"):
            self.window.find_element("missing")
        with self.assertRaises(ElementAmbiguousError) as raised:
            self.window.find_element(control_type="Button")
        self.assertEqual(len(raised.exception.elements), 2)

    def test_discovery_waits_and_can_retry_transient_ambiguity(self):
        self.window = Automation(
            timeout_seconds=1,
            settle_seconds=0,
            retry_on_ambiguity=True,
            dpi_awareness="unchanged",
        ).window(99)
        with (
            patch.object(
                self.root, "elements", side_effect=[(), (self.run, self.ok), (self.ok,)]
            ),
            patch("litewinwrap.elements.time.sleep"),
        ):
            self.assertEqual(self.window.find_element().name, "OK")

    def test_explicit_zero_timeout_and_false_retry_override_policy(self):
        self.window = Automation(
            timeout_seconds=10, retry_on_ambiguity=True, dpi_awareness="unchanged"
        ).window(99)
        with (
            self.assertRaises(ElementNotFoundError),
            patch("litewinwrap.elements.time.sleep") as sleep,
        ):
            self.window.find_element("missing", timeout_seconds=0)
        sleep.assert_not_called()
        with (
            self.assertRaises(ElementAmbiguousError),
            patch("litewinwrap.elements.time.sleep") as sleep,
        ):
            self.window.find_element(retry_on_ambiguity=False)
        sleep.assert_not_called()


class NativeErrorTests(unittest.TestCase):
    def test_hresult_classification_preserves_native_error(self):
        _uia._check(0, "success")
        with self.assertRaises(ElementUnavailableError) as raised:
            _uia._check(-2147220991, "CurrentName")
        self.assertEqual(raised.exception.hresult, 0x80040201)
        with self.assertRaises(UIAutomationError) as raised:
            _uia._check(-2147024891, "Invoke")
        self.assertEqual(raised.exception.hresult, 0x80070005)
        self.assertIn("0x80070005", str(raised.exception))

    def test_native_uia_is_lazy_and_windows_only(self):
        with patch("litewinwrap._uia.sys.platform", "linux"):
            with self.assertRaisesRegex(OSError, "requires Windows"):
                _uia.from_handle(123)


if __name__ == "__main__":
    unittest.main()
