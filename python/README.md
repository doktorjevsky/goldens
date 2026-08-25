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

import cv2

from litewinwrap import Target, Window, keyboard, match, win32

win32.enable_per_monitor_dpi_awareness()

process = subprocess.Popen([r"C:\path\to\application.exe", "--example-argument"])
window = Window.find(
    re.compile("Application title", re.IGNORECASE),
    process_id=process.pid,
    timeout=5.0,
)
window.focus()

pixels = cv2.imread("button_8.png", cv2.IMREAD_COLOR)
eight = Target("button_8", pixels, click=(0.5, 0.5))
found = match.click(window, eight, threshold=0.92, timeout=2.0)

print(found.score, found.rect, found.click)
keyboard.write("123")
```

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

See [`examples/steer_window.py`](examples/steer_window.py) for a runnable script.
Its optional `--start` and repeatable `--start-arg` arguments launch the target
before looking for its window.

## Tests

The platform-independent matching and value-behaviour tests run on any system
with the package dependencies installed:

```powershell
$env:PYTHONPATH = "src"
py -m unittest discover -s tests -v
```

Live window, capture, and input calls require an interactive Windows desktop.
