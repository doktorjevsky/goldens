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

from litewinwrap import Goldens, Window, win32

win32.enable_per_monitor_dpi_awareness()

subprocess.Popen(["calc.exe"])
window = Window.find(
    re.compile("Calculator", re.IGNORECASE),
    timeout=5.0,
)
window.focus()

targets = Goldens("calculator.png")
found = window.click_target(targets["button_8"], threshold=0.92, timeout=2.0)

print(found.score, found.rect, found.click)
```

`Goldens` eagerly reads the PNG and adjacent JSON sidecar. It validates every
annotation and exposes copied, read-only crops through normal mapping operations:

```python
targets["button_8"]
targets.get("button_8")
"button_8" in targets
targets.keys()
targets.items()
```

`Window.screenshot()`, `find_target()`, `find_targets()`, and `click_target()`
take fresh screenshots and retain no target or match state on the window.
`find_target()` and `click_target()` require exactly one match and report an
ambiguity if the target occurs more than once. Use `find_targets()` when every
occurrence is wanted. Choosing the highest-scoring occurrence is deliberately
explicit through `find_best_target()` or `click_best_target()`.

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

## Tests

The platform-independent matching and value-behaviour tests run on any system
with the package dependencies installed:

```powershell
$env:PYTHONPATH = "src"
py -m unittest discover -s tests -v
```

Live window, capture, and input calls require an interactive Windows desktop.
