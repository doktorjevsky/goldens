# Goldens

Goldens is a dependency-free native Windows 11 application for creating and
annotating screenshot resources used by pixel-based regression tests.

Each resource consists of a visible PNG and an optional adjacent JSON file:

```text
login.png
login.json
```

The JSON stores uniquely named rectangular annotations in image pixels and an
optional click point normalized within its rectangle.

## Build

Install or unpack LLVM-MinGW, ensure `x86_64-w64-mingw32-clang.exe` is on
`PATH`, and run:

```bat
build.bat
```

The standalone application is written in C17 and uses only Windows system
libraries. The executable is produced at `build\goldens.exe`.

Goldens opens the process working directory as its resource root at startup, so
running it from a resource directory requires no setup:

```bat
cd C:\path\to\resources
C:\path\to\goldens.exe
```

You can also pass a directory as the first argument or switch roots with
**File → Open Folder…**. The status strip always shows the directory where new captures will
be saved. Selecting a directory or a PNG in the left tree changes that capture
destination to the corresponding directory.

## Interface

- **File**, **Edit**, **View**, and **Capture** menus contain document and
  application commands. These are deliberately separate from image tools.
- The compact horizontal tool row above the editor uses the official Microsoft
  Fluent System Icons for
  **Select**, **Rectangle**, **Click**, and **Hand**. Their antialiased masks are
  rendered at the current DPI and tinted for each button state; native hover
  tooltips and accessible button names remain available. Select moves
  annotations and resizes from the lower-right handle.
  Rectangle draws a new boundary and immediately asks for its unique name.
  After a successful creation Goldens returns to Select. Click is disabled until
  an annotation is selected and only accepts points inside that annotation.
  While drawing, a high-opacity orange tint and thick yellow outline keep the
  new boundary visible over both dark and light images.
  Click assigns a normalized click point to the annotation under the cursor.
  Hand pans the canvas; middle-drag also pans from any tool.
- The resource tree expands the active PNG to show its annotations. Selection
  stays synchronized in both directions between the tree and editor. Boundary
  names appear as hover tooltips instead of labels over the image. Double-click
  a PNG or annotation name to rename it in place; Enter commits and Escape
  cancels. Resource renames move the PNG and JSON sidecar as one transaction.
  Clicking blank space in either tree deselects that source. Deselecting an
  annotation first returns selection to its parent PNG. If a displayed PNG or
  window is deselected, the editor falls back to the source selected in the
  other tree or becomes empty.
- Changes are written only by **File → Save Annotations**. Switching images or exiting prompts if
  there are unsaved changes.
- Select a window in the right column to start a continuously refreshed,
  asynchronous bitmap preview in the center, then use **Capture** or
  **Recapture** beside the window list. Preview mode selects **Hand** and
  disables the annotation tools until an editable resource is selected again.
  Capture is available only for a selected window; Recapture additionally
  requires a selected PNG or one of its annotations.
  Open, closed, minimized, and renamed windows are reconciled automatically.
- The contextual strip says either **Editing resource** or **Previewing
  window**; generic panel-title rows have been removed.
- Use **Fit**, **−**, **+**, or the mouse wheel to control zoom. **View → Actual
  Size** (or **1**) switches to 100%. Drag a preview, or middle-drag an image,
  to pan. At 100%, image pixels are mapped exactly to
  display pixels; scaled views use halftone filtering. The editor is
  double-buffered so boundaries remain above the image throughout a drag.
- The three-column layout is responsive and DPI-aware. Controls are repositioned
  in a single redraw pass, context text is ellipsized before the zoom controls,
  and the minimum window size prevents the capture buttons from overlapping.
- Drag either vertical divider to resize the Resources or Windows column. Drag
  a pane 48 pixels beyond its minimum width to collapse it; drag its remaining
  visible edge strip inward to restore it. The strip has a wider internal hit
  area so it does not compete with the outer window-resize border. Double-clicking
  the divider also toggles the pane as a fallback. The editor uses the released space.

## Tests

Run `test.bat`. The native test suite covers annotation names and geometry,
click normalization, viewport transforms, multi-annotation JSON round trips,
live top-level window lifecycle reconciliation (including minimized windows),
exact 1:1 GDI rendering, overlay ordering, custom and collapsed column layouts
from compact to wide and 100–200% DPI, version-compatible native tooltip
registration and positioning, DPI-scaled tool icon rasterization, transactional
PNG/JSON renames with post-rename saves, lossless BGRA PNG encode/decode, and
the window-preview allocation/copy pipeline.

Run `install-hooks.bat` once after cloning. It configures the versioned
`.githooks` directory. Both pre-commit and pre-push run the complete Windows
suite and block the Git operation if any test fails.

## Python scripting prototype

The [`python`](python) subproject contains a first-pass Windows scripting layer
for live window discovery, child-window enumeration, mouse and keyboard input,
OpenCV template matching, and loading PNG/JSON resources as target mappings.
The native Goldens application remains dependency-free.
