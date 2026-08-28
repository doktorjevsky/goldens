@echo off
setlocal

pushd "%~dp0.."

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

for %%S in (model render tool_icon ui_layout tooltip resource_tree resource_ops resource_watcher history atomic_file png_io scene_capture) do (
  if /i "%~1"=="%%S" goto suite_%%S
)

echo Unknown test suite: %~1 1>&2
exit /b 2

:suite_model
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\model_tests.c src\model.c src\document.c -o build\model_tests.exe -luser32
if errorlevel 1 exit /b 1
build\model_tests.exe
exit /b %errorlevel%

:suite_render
%CC% -std=c17 -O2 -Wall -Wextra tests\render_tests.c src\editor_render.c ^
  -o build\render_tests.exe -lgdi32 -lmsimg32 -luser32
if errorlevel 1 exit /b 1
build\render_tests.exe
exit /b %errorlevel%

:suite_tool_icon
%CC% -std=c17 -O2 -Wall -Wextra tests\tool_icon_tests.c src\ui_tool_icon.c ^
  -o build\tool_icon_tests.exe -lgdi32 -lmsimg32 -luser32
if errorlevel 1 exit /b 1
build\tool_icon_tests.exe
exit /b %errorlevel%

:suite_ui_layout
%CC% -std=c17 -O2 -Wall -Wextra tests\ui_layout_tests.c src\ui_layout.c ^
  -o build\ui_layout_tests.exe -luser32
if errorlevel 1 exit /b 1
build\ui_layout_tests.exe
exit /b %errorlevel%

:suite_tooltip
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\tooltip_tests.c src\ui_tooltip.c ^
  -o build\tooltip_tests.exe -lcomctl32 -luser32
if errorlevel 1 exit /b 1
build\tooltip_tests.exe
exit /b %errorlevel%

:suite_resource_ops
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\resource_ops_tests.c src\resource_ops.c src\atomic_file.c src\document.c src\model.c ^
  -o build\resource_ops_tests.exe -luser32
if errorlevel 1 exit /b 1
build\resource_ops_tests.exe
exit /b %errorlevel%

:suite_resource_tree
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\resource_tree_tests.c src\resource_tree.c ^
  -o build\resource_tree_tests.exe -lcomctl32 -luser32
if errorlevel 1 exit /b 1
build\resource_tree_tests.exe
exit /b %errorlevel%

:suite_resource_watcher
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\resource_watcher_tests.c src\resource_watcher.c ^
  -o build\resource_watcher_tests.exe -luser32
if errorlevel 1 exit /b 1
build\resource_watcher_tests.exe
exit /b %errorlevel%

:suite_history
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\history_tests.c src\history.c ^
  -o build\history_tests.exe -luser32
if errorlevel 1 exit /b 1
build\history_tests.exe
exit /b %errorlevel%

:suite_atomic_file
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\atomic_file_tests.c src\atomic_file.c ^
  -o build\atomic_file_tests.exe -luser32
if errorlevel 1 exit /b 1
build\atomic_file_tests.exe
exit /b %errorlevel%

:suite_png_io
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS -municode ^
  -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK ^
  tests\png_io_tests.c tests\third_party\lodepng\lodepng.c ^
  src\image_io.c src\atomic_file.c ^
  -o build\png_io_tests.exe ^
  -lole32 -luuid -lwindowscodecs
if errorlevel 1 exit /b 1
build\png_io_tests.exe
exit /b %errorlevel%

:suite_scene_capture
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\scene_capture_tests.c src\scene_capture.c ^
  src\image_io.c src\atomic_file.c src\resource_ops.c ^
  -o build\scene_capture_tests.exe ^
  -ldwmapi -lgdi32 -luser32 -lole32 -luuid -lwindowscodecs
if errorlevel 1 exit /b 1
build\scene_capture_tests.exe
exit /b %errorlevel%
