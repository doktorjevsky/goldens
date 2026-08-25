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

import cv2

from litewinwrap import Target, Window, keyboard, match, win32

win32.enable_per_monitor_dpi_awareness()

window = Window.find(re.compile("Calculator", re.IGNORECASE), timeout=2.0)
window.focus()

pixels = cv2.imread("button_8.png", cv2.IMREAD_COLOR)
eight = Target("button_8", pixels, click=(0.5, 0.5))
found = match.click(window, eight, threshold=0.92, timeout=2.0)

print(found.score, found.rect, found.click)
keyboard.write("123")
```

`match.click` captures the window from the physical screen, matches the cropped
annotation, sends a click to its saved normalized click point, and returns the
`Match` that was clicked.

Screen capture intentionally reflects what is visible on the desktop. Keep the
interactive desktop unlocked and the target window unobscured.

See [`examples/steer_window.py`](examples/steer_window.py) for a runnable script.

## Tests

The platform-independent matching and value-behaviour tests run on any system
with the package dependencies installed:

```powershell
$env:PYTHONPATH = "src"
py -m unittest discover -s tests -v
```

Live window, capture, and input calls require an interactive Windows desktop.
