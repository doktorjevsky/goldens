# litewinwrap

`litewinwrap` is a small Windows-only scripting layer for finding windows,
querying their current properties, injecting mouse and keyboard input, and
finding visual templates with OpenCV.

`Window` retains only its HWND. Titles, rectangles, visibility, process IDs,
parents, and children are always queried from Windows when accessed.

## Install

From this directory:

```powershell
py -m pip install -e .
```

## Shape of the API

```python
import re
import subprocess
from pathlib import Path

from litewinwrap import Goldens, Window, win32

win32.enable_per_monitor_dpi_awareness()

subprocess.Popen(["calc.exe"])
window = Window.find(
    re.compile("Calculator", re.IGNORECASE),
    timeout=5.0,
)
window.focus()

targets = Goldens.from_root(Path("resources"))
found = window.click_target(
    targets["calculator/button_8"],
    threshold=0.92,
    timeout=2.0,
)

print(found.score, found.rect, found.click)
```

`Goldens.from_root()` recursively discovers annotated PNGs below a resource
root. Each target is qualified by the PNG's root-relative path without its
extension, using `/` on every platform. `Goldens.from_png()` loads one PNG/JSON
pair using the PNG stem as that same namespace. Both forms eagerly validate the
resources and expose copied, read-only crops through normal mapping operations:

```python
targets["calculator/button_8"]
targets.get("calculator/button_8")
"calculator/button_8" in targets
targets.keys()
targets.items()
```

For example, an annotation named `submit` in
`resources/dialogs/login.png` has the identifier `dialogs/login/submit`.
PNG files without an adjacent JSON sidecar are not targets and are skipped by
root discovery. Annotation names cannot contain `/`, and identifiers that differ
only by case are rejected so the mapping remains safe on Windows filesystems.

`Window.screenshot()`, `find_target()`, `find_targets()`, and `click_target()`
take fresh screenshots and retain no target or match state on the window.
`find_target()` and `click_target()` require exactly one match and report an
ambiguity if the target occurs more than once. Use `find_targets()` when every
occurrence is wanted. Choosing the highest-scoring occurrence is deliberately
explicit through `find_best_target()` or `click_best_target()`.

For transient ambiguity caused by animations or other changing UI, pass
`retry_on_ambiguity=True` together with a timeout. The operation then keeps
capturing until exactly one match remains. If the last capture is still
ambiguous at the deadline, it raises `TargetAmbiguousError` with those matches:

```python
window.click_target(
    targets["calculator/button_8"],
    timeout=2.0,
    retry_on_ambiguity=True,
)
```

The equivalent operations on an existing capture are `match.match()`,
`match.match_all()`, and `match.best_match()`.

`subprocess.Popen` is part of Python's standard library and is deliberately not
wrapped. Pass an argument list to launch an executable directly. The returned
object exposes the PID, exit status, `wait`, `terminate`, and the standard I/O
streams. `Window.find(..., process_id=process.pid)` can wait for the process to
create its top-level window.

`cmd.exe` is unnecessary for ordinary executables. Invoke it explicitly only
when the command actually needs shell syntax or a built-in command:

```python
subprocess.Popen(["cmd.exe", "/d", "/s", "/c", "your shell command"])
```

Avoid `shell=True` for values assembled from external input.

`match.click` captures the window from the physical screen, matches the cropped
annotation, sends a click to its saved normalized click point, and returns the
`Match` that was clicked.

Screen capture intentionally reflects what is visible on the desktop. Keep the
interactive desktop unlocked and the target window unobscured.

See [`examples/calculator.py`](examples/calculator.py) for a complete script that
starts Calculator, loads `calculator.png` and its JSON sidecar, and exercises
every digit and arithmetic-operation target.

[`examples/calculator_not_found.py`](examples/calculator_not_found.py) changes
Calculator's display and then demonstrates the structured diagnostics returned
by `TargetNotFoundError` when the original visual state does not reappear before
the timeout.

[`examples/calculator_multiple_match.py`](examples/calculator_multiple_match.py)
presses `1` six times using targets from `calculator.png`, then demonstrates
`TargetAmbiguousError` using `multiple_match` from `calculator_ones.png`.

## Tests

The platform-independent matching and value-behaviour tests run on any system
with the package dependencies installed:

```powershell
$env:PYTHONPATH = "src"
py -m unittest discover -s tests -v
```

Live window, capture, and input calls require an interactive Windows desktop.
