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
libraries. The executable is produced at `build\goldens.exe`. Conversion,
format, shadowing, and prototype diagnostics are enabled and treated as build
errors.

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
  **Select**, **Rectangle**, and **Click**. Their antialiased masks are
  rendered at the current DPI and tinted for each button state; native hover
  tooltips and accessible button names remain available. Select moves
  annotations, resizes from the lower-right handle, and pans when dragging the
  image background. The arrow cursor changes to four-way arrows while moving an
  annotation or panning, and to a diagonal resize cursor over an active resize.
  Rectangle draws a new boundary, assigns it a unique default name, and opens
  that name for immediate in-place editing in the resource tree. Rectangle stays
  active after creation so several boundaries can be drawn in sequence. Click is
  disabled until an annotation is selected and only accepts points inside it.
  While drawing, a high-opacity orange tint and thick yellow outline keep the
  new boundary visible over both dark and light images.
  Click assigns or repositions a normalized click point in the selected
  annotation. Once a point exists and Click is still active, its target icon
  changes to a target-with-X; clicking the tool button again clears the point
  while leaving Click active so a replacement can be placed immediately.
  Middle-drag also pans from any tool.
- The resource tree expands the active PNG to show its annotations. Selection
  stays synchronized in both directions between the tree and editor. Boundary
  names appear as hover tooltips instead of labels over the image. Double-click
  a PNG or annotation name to rename it in place; Enter commits and Escape
  cancels. Use **File → New Folder…** or **Ctrl+N** to create a folder beneath
  the selected resource directory. F2 also renames non-root folders. Drag PNGs
  and folders onto a directory to move them within the open resource root;
  annotation rows cannot be dragged. PNG moves keep the same-stem JSON sidecar
  attached. If a move or rename would replace a PNG, Goldens asks first with
  **No** as the default; accepting replaces its sidecar too, and **Ctrl+Z**
  restores both the moved resource and the replaced pair. Delete removes a
  selected PNG and its sidecar together, or—after confirmation—a non-root
  folder and its complete contents. **Ctrl+Z** restores the PNG pair or folder
  tree, and **Ctrl+Y** deletes it again. **Ctrl+C** copies the
  displayed image to the Windows clipboard. When it is a PNG resource, pasting
  it with **Ctrl+V** duplicates the PNG and sidecar under an available name;
  clashes are indexed as `name-1.png`, `name-2.png`, and so on. Bitmap
  images copied from other applications can also be pasted as new named PNG
  resources. Resource-folder expansion is preserved across refreshes, and the
  paste destination opens automatically. Pasted resources participate in
  undo/redo like new captures.
  A persistent transaction journal completes or rolls back an
  interrupted pair move on refresh or next startup, including the case where
  Windows could not perform the immediate PNG rollback. Moves never
  merge folders or silently overwrite an existing PNG, folder, or JSON sidecar. Folder
  creation, capture/recapture, and PNG/folder moves or renames join annotation edits in the ordered
  **Ctrl+Z**/**Ctrl+Y** undo history. An undo blocked by an externally created
  collision or a now-nonempty folder remains retryable after the conflict is fixed.
  The history retains the latest 32 actions. Annotation snapshots allocate only
  the entries that need them, and recoverable capture versions are deleted when
  their entry is evicted, replaced by a new branch, or the application exits.
  Clicking blank space in either tree deselects that source. Deselecting an
  annotation first returns selection to its parent PNG. If a displayed PNG or
  window is deselected, the editor falls back to the source selected in the
  other tree or becomes empty.
- The resource tree watches the open folder and all of its subfolders. PNGs and
  directories added, moved, renamed, or removed in Explorer appear
  automatically, while expanded folders and the current selection are
  preserved. An active PNG follows external moves and renames within the open
  resource folder. PNGs and non-root directories can also be dragged onto a
  directory in the tree to move them; a PNG's JSON sidecar moves with it.
- Changes are written only by **File → Save Annotations**. JSON and captured
  PNG files are replaced atomically, so an interrupted or failed write leaves
  the last valid file in place. Switching images or exiting prompts if there
  are unsaved changes. Save is grayed while annotations are up to date. Menu
  commands are refreshed whenever a menu opens: Undo/Redo, annotation
  rename/delete/click clearing, image view controls, resource refresh and
  folder/capture operations are grayed whenever their required selection or
  resource is unavailable.
- Select a window in the right column to start a continuously refreshed,
  asynchronous bitmap preview in the center, then use **Capture** or
  **Recapture** beside the window list. Preview mode uses **Select** for panning and
  disables the annotation tools until an editable resource is selected again.
  A persistent worker coalesces stale requests and alternates between two
  reusable capture surfaces, avoiding a thread, bitmap allocation, and pixel
  copy for every frame. Preview refresh and window enumeration pause while
  panning so pointer movement remains responsive. If the preview service cannot
  start, Goldens reports the failure and closes instead of silently leaving
  preview controls inoperative.
  Capture is available only for a selected window; Recapture additionally
  requires a selected PNG or one of its annotations.
  Open, closed, minimized, and renamed windows are reconciled automatically.
- The contextual strip says either **Editing resource** or **Previewing
  window**; generic panel-title rows have been removed.
- Use **Fit**, **−**, **+**, or the mouse wheel to control zoom. Wheel zoom keeps
  the image point beneath the mouse pointer in place. **View → Actual
  Size** (or **1**) switches to 100%. With Select, drag an image background or
  preview to pan; middle-drag also works from any tool. At 100%, image pixels are mapped exactly to
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

Run `test.bat`. It first builds the shipped application with the strict
production diagnostics, then compiles and runs the independent native test
suites in parallel. By default it runs two jobs at a time; set
`GOLDENS_TEST_JOBS` to a positive integer to adjust the concurrency. The
suite covers
annotation names and geometry,
click normalization, viewport transforms, strict and order-independent JSON
parsing, arbitrarily long unknown keys, Unicode and escape round trips,
malformed/truncated input, numeric and nesting limits, full annotation capacity,
and invalid-model serialization,
live top-level window lifecycle reconciliation (including minimized windows),
recursive resource-folder change notifications, burst coalescing, stress
coverage, and clean watcher shutdown,
resource-tree move validation and PNG/JSON moves between directories,
exact 1:1 GDI rendering, overlay ordering, custom and collapsed column layouts
from compact to wide and 100–200% DPI, version-compatible native tooltip
registration and positioning, DPI-scaled tool icon rasterization, transactional
PNG/JSON copy and rename/move undo/redo round trips including persistent-journal
recovery after rollback failure, mixed ring-buffer history ordering, branching,
capacity eviction, failed-action retry, and saved-state comparison,
folder create/move/delete undo/redo round trips, staged-tree cleanup and subtree
rejection, orphan-sidecar
collision safety, atomic-write failure preservation,
lossless BGRA PNG encode/decode and replacement, and reusable preview capture
surfaces with removal of invisible resize-border pixels from `PrintWindow`
frames. PNG coverage includes all 161 valid PngSuite images, comparison with the
independent LodePNG decoder, encoded signature/chunk-order/CRC validation,
padded-stride and malformed-input cases, and exact lossless BGRA/alpha fidelity.
Preview coverage includes latest-request coalescing, stale-result rejection,
persistent-worker reuse, failure recovery, and bounded preview shutdown.

Run `install-hooks.bat` once after cloning. It configures the versioned
`.githooks` directory. Both pre-commit and pre-push run the complete Windows
suite and block the Git operation if any test fails.

On a non-Windows development machine, keep machine names, credentials, and
remote paths out of the repository by configuring a private executable runner:

```sh
git config --local goldens.windowsTestRunner /absolute/path/to/private-runner
```

`GOLDENS_WINDOWS_TEST_RUNNER` may be used instead. The hook passes the local
repository root as the runner's only argument. The runner must transfer the
exact working tree, including uncommitted changes, to Windows and return the
exit status from `test.bat`; testing a separate unsynchronized checkout does
not validate a commit.

Run `benchmark.bat` to compare the original allocate-and-rescale paint path
with the retained-buffer and scaled-image-cache paths used while panning.

For adversarial JSON conformance coverage, clone the MIT-licensed
[JSONTestSuite](https://github.com/nst/JSONTestSuite) at commit
`1ef36fa01286573e846ac449e8683f8833c5b26a`, then run
`json-corpus.bat C:\path\to\JSONTestSuite`. The runner checks all 95
must-accept and 188 must-reject cases; the suite's 35 implementation-defined
cases are counted and reported but deliberately not treated as pass/fail.

## Python scripting prototype

The [`python`](python) subproject contains a first-pass Windows scripting layer
for live window discovery, child-window enumeration, mouse and keyboard input,
OpenCV template matching, and loading individual PNG/JSON pairs or recursive
resource roots as mappings of path-qualified targets.
The native Goldens application remains dependency-free.
