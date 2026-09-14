"""Inspect and operate the external Windows UI Test App."""

from __future__ import annotations

from litewinwrap import Automation


automation = Automation(timeout_seconds=5.0)
window = automation.find_window(class_name="WindowsUIAutomationTestApp")

for element in window.elements():
    print(element)

window.find_element(automation_id="1001", control_type="Edit").set_value("Example task")
window.find_element(automation_id="1002", control_type="Edit").set_value(
    r"C:\Temp\Output"
)
window.find_element(automation_id="1003", control_type="CheckBox").set_checked(True)
window.find_element(automation_id="1004", control_type="CheckBox").set_checked(False)
window.find_element("Run", control_type="Button").invoke()
print(window.find_element(automation_id="1007").value)
