# litewinwrap

`litewinwrap` is a small image-driven Windows desktop automation package. It
finds windows, locates annotated visual targets, and performs mouse and keyboard
actions with one consistent timeout and settling policy.

The package captures the visible desktop. It is intended for interactive
Windows sessions where the target application is unlocked and unobscured.

## Install

This is an alpha release. APIs may change before `1.0`.

Copy the wheel from `dist` to the Windows VM, then install it into a virtual
environment:

```powershell
py -m venv C:\Tools\litewinwrap-env
C:\Tools\litewinwrap-env\Scripts\python -m pip install `
    C:\Transfer\litewinwrap-0.1.0a9-py3-none-any.whl
```

For editable development from this directory:

```powershell
py -m pip install -e .
```

Python 3.11 or later and Windows 11 are required. Creating an `Automation`
session enables per-monitor DPI awareness before window geometry is queried.

## Quick start

```python
from __future__ import annotations

import re
import subprocess
from pathlib import Path

from litewinwrap import Automation, Goldens


automation = Automation(
    timeout_seconds=2.0,
    settle_seconds=0.15,
    threshold=0.92,
    focus_before_input=True,
)

subprocess.Popen(["calc.exe"])
calculator = automation.find_window(
    re.compile(r"^Calculator$", re.IGNORECASE),
    timeout_seconds=5.0,
)
targets = Goldens.from_png(Path("calculator.png"))

calculator.focus()
calculator.click(targets["calculator/button_1"])
calculator.click(targets["calculator/button_plus"])
calculator.click(targets["calculator/button_2"])
calculator.press("enter")
```

The session is the policy owner. Every `Window` returned by it inherits the
same defaults:

- `timeout_seconds`: how long discovery and visual searches may retry;
- `settle_seconds`: how long actions yield for the UI to repaint afterward;
- `threshold` and `overlap`: visual-matching defaults;
- `retry_on_ambiguity`: whether a transient multiple match may be retried;
- `focus_before_input`: whether clicks and keyboard actions activate their window.

An option supplied to an individual action overrides the session. Explicit
zero disables waiting:

```python
calculator.click(target, timeout_seconds=4.0, settle_seconds=0.3)
calculator.click(target, settle_seconds=0.0)
calculator.press("ctrl", "q", focus=False)
```

`None` means “inherit the session.” Explicit values—including `0.0` and
`False`—always override it for that one call. DPI awareness is necessarily a
process-level choice rather than a per-action setting; applications that
already manage it can opt out with `Automation(dpi_awareness="unchanged")`.
All time-bearing API values include their unit in the name; they currently use
seconds, for example `timeout_seconds`, `settle_seconds`, and `interval_seconds`.

## Windows

Discovery begins on the session:

```python
automation.windows()
automation.find_windows(title="Calculator")
automation.find_window(title="Calculator")
```

`find_windows()` is an immediate plural query. `find_window()` waits for
exactly one result, raising `WindowNotFoundError` or `WindowAmbiguousError`
otherwise.

A `Window` retains its HWND and originating automation session. Properties are
always read live from Windows:

```python
window.title
window.class_name
window.rect
window.process_id
window.parent
window.children()
window.visible
window.enabled
window.minimized
window.foreground
```

Window state actions use the session settling policy:

```python
window.focus()
window.move(100, 100)
window.resize(900, 700)
window.restore()
window.minimize()
window.maximize()
window.close()
```

## Visual targets

The native Goldens application creates a PNG and adjacent JSON sidecar. The
sidecar stores named annotation rectangles and optional normalized click points:

```text
login.png
login.json
```

`Goldens.from_png()` validates one pair and qualifies every annotation with the
PNG stem:

```python
targets = Goldens.from_png("login.png")
submit = targets["login/submit"]
```

`Goldens.from_root()` recursively discovers annotated PNGs below a resource
root. Each identifier uses the PNG's root-relative path without its extension,
with `/` on every platform:

```python
targets = Goldens.from_root(Path("resources"))
submit = targets["dialogs/login/submit"]
```

Both forms expose copied, read-only crops through normal mapping operations.
PNG files without an adjacent JSON sidecar are skipped during root discovery.
Annotation names cannot contain `/`, and identifiers differing only by case are
rejected so the mapping remains safe on Windows filesystems.

The primary visual operations take fresh captures and retain no target or match
state on the window:

```python
capture = window.capture()
match = window.locate(submit)
matches = window.locate_all(submit)
best = window.locate_best(submit)
hovered = window.hover(submit)
clicked = window.click(submit)
clicked_best = window.click_best(submit)
```

`locate()` and `click()` require exactly one distinct occurrence. Choosing the
highest-scoring occurrence is deliberately explicit. A failed search raises
`TargetNotFoundError` with the best score and last capture; multiple occurrences
raise `TargetAmbiguousError`. `hover()` moves to the target's annotated click
point, or its center when no click point was saved, then uses the session's
settling policy so hover-driven UI has time to render.

## Text and keys

High-level keyboard actions focus the bound window when configured to do so and
then use the session settling policy:

```python
window.type_text("Hello, världen 👋")
window.press("enter")
window.press("tab", count=3)
window.press("ctrl", "q")
window.press("ctrl", "shift", "s")
```

Key names are case-insensitive. Letters, digits, modifiers, navigation keys,
F1–F24, numpad keys, and common media keys are supported. Multiple names form a
chord and are released in reverse order.

## Advanced primitives

The high-level workflow above is the supported default. The underlying modules
remain available when exact device or matching control is required:

```python
from litewinwrap import keyboard, match, mouse, win32

keyboard.type_text("literal Unicode")
keyboard.press("ctrl", "q")
mouse.click((100, 200))
mouse.move_to((300, 200), duration_seconds=0.2)
mouse.move_by(100, 0, duration_seconds=0.2)
mouse.drag_to((500, 200))
mouse.drag_by(100, 0)
match.match(capture, target)
win32.get_window_rect(hwnd)
```

These primitives do not all establish focus or inherit an `Automation`
session. Integer Win32 virtual-key codes and explicit key down/up operations are
available here as escape hatches. Context managers compose modifiers and pointer
gestures without building special cases into every action:

```python
window.hover(submit)
with keyboard.hold("ctrl"):
    mouse.drag_by(100, 0)

with keyboard.hold("ctrl", "shift"):
    with mouse.hold("left"):
        mouse.move_by(100, 0, duration_seconds=0.25)
```

Both `hold()` context managers release their keys or button even if the block
raises. For unusual interactions, `keyboard.down()` / `keyboard.up()` and
`mouse.button_down()` / `mouse.button_up()` remain fully explicit.

`move_to()` and `move_by()` are instantaneous unless `duration_seconds` is
provided. Timed movement emits intermediate absolute positions at 60 Hz, so
relative distances remain exact and are not changed by the Windows pointer
acceleration setting. Drags default to `duration_seconds=0.25` so applications
receive a useful stream of movement events while the button is held. Pass
`duration_seconds=0.0` for an explicitly instantaneous drag.

`subprocess.Popen` remains the normal way to launch applications. Pass an
argument list and avoid `shell=True` for values assembled from external input.

## Failure reports

`Reports` adds runner-independent, per-test failure artifacts without changing
normal automation calls:

```python
from pathlib import Path

from litewinwrap import Reports


reports = Reports(
    Path("artifacts"),
    max_frames=40,
    recording_interval_seconds=0.2,
)


@reports.test(
    title="Create and save a document",
    owner="editor-team",
    purpose="Verify the complete save workflow",
)
def test_save_document() -> None:
    with reports.step("Create a blank document"):
        window.click(new_document)

    with reports.step("Save the document"):
        window.click(save)
        window.locate(saved_confirmation)


if __name__ == "__main__":
    test_save_document()
```

On failure, the decorator writes one static directory for that test containing
`index.html`, `trace.json`, `failure.gif`, `failure.png`, and its action-boundary
frames. A visual matching failure also includes `target.png`, the exact target
that could not be found. It then re-raises the original exception, preserving
normal script and CI failure behavior. Passing tests write no artifact.

The report page leads with the error, missing target, screen at failure, and an
interactive frame player. It shows the same matching rectangles, click markers,
and action labels as the GIF. The player supports play/pause, previous/next,
scrubbing, and speeds from 0.25× to 2×. It works offline with no server or
external assets. Images are constrained to the browser viewport and link to
their full-size files. `failure.gif` remains available as a portable fallback.
Traceback and action details are collapsed; the complete machine-readable trace
remains in `trace.json`. High-level window operations and direct matching,
mouse, and keyboard primitives are traced automatically.

Technical details present steps as one numbered, chronological timeline. Each
step contains its actions in execution order; completed steps have a green
checkmark and the failed step is marked in red.

While a window is active, reports sample it every
`recording_interval_seconds=0.2` (up to 5 captured frames per second). Playback
speed is selected in the HTML report rather than configured in Python. GIFs are
downscaled and palette-compressed to keep artifact sizes reasonable.

Literal text passed to `type_text()` or `write()` is never recorded; reports
store only its character count. Window screenshots can still contain sensitive
content. Disable them for a whole test or one step when required:

```python
@reports.test(title="Enter a secret", capture_frames=False)
def test_secret() -> None:
    ...

with reports.step("Enter a secret", capture_frames=False):
    window.type_text(secret)
```

Reports keep at most `max_frames` and are written only for failures in this
alpha. They use only local files and require no server or external web assets.

## Tests

Platform-independent policy, matching, format, and delegation tests run on any
system with the package dependencies installed:

```powershell
$env:PYTHONPATH = "src"
py -m unittest discover -s tests -v
```

Live window, capture, and input calls require an interactive Windows desktop.
The complete Calculator workflows are in [`examples`](examples).

Building, transferring, online installation, and fully offline VM installation
are documented in [`INSTALLING.md`](INSTALLING.md).
