@echo off
setlocal

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

if not exist build mkdir build

call build.bat build\goldens-test.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\model_tests.c src\model.c src\document.c -o build\model_tests.exe -luser32
if errorlevel 1 exit /b 1

build\model_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\window_tracker_tests.c src\window_tracker.c src\model.c ^
  -o build\window_tracker_tests.exe -luser32
if errorlevel 1 exit /b 1
build\window_tracker_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra tests\render_tests.c src\editor_render.c ^
  -o build\render_tests.exe -lgdi32 -lmsimg32 -luser32
if errorlevel 1 exit /b 1
build\render_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra tests\tool_icon_tests.c src\ui_tool_icon.c ^
  -o build\tool_icon_tests.exe -lgdi32 -lmsimg32 -luser32
if errorlevel 1 exit /b 1
build\tool_icon_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra tests\ui_layout_tests.c src\ui_layout.c ^
  -o build\ui_layout_tests.exe -luser32
if errorlevel 1 exit /b 1
build\ui_layout_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\tooltip_tests.c src\ui_tooltip.c ^
  -o build\tooltip_tests.exe -lcomctl32 -luser32
if errorlevel 1 exit /b 1
build\tooltip_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\resource_ops_tests.c src\resource_ops.c src\document.c src\model.c ^
  -o build\resource_ops_tests.exe -luser32
if errorlevel 1 exit /b 1
build\resource_ops_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\resource_watcher_tests.c src\resource_watcher.c ^
  -o build\resource_watcher_tests.exe -luser32
if errorlevel 1 exit /b 1
build\resource_watcher_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE ^
  tests\atomic_file_tests.c src\atomic_file.c ^
  -o build\atomic_file_tests.exe -luser32
if errorlevel 1 exit /b 1
build\atomic_file_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS -municode ^
  -DLODEPNG_NO_COMPILE_ENCODER -DLODEPNG_NO_COMPILE_DISK ^
  tests\png_io_tests.c tests\third_party\lodepng\lodepng.c ^
  src\image_io.c src\atomic_file.c ^
  -o build\png_io_tests.exe ^
  -lole32 -luuid -lwindowscodecs
if errorlevel 1 exit /b 1
build\png_io_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\preview_capture_tests.c src\preview_capture.c src\image_io.c src\atomic_file.c ^
  -o build\preview_capture_tests.exe -ldwmapi -lgdi32 -luser32 -lole32 -luuid -lwindowscodecs
if errorlevel 1 exit /b 1
build\preview_capture_tests.exe
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  tests\preview_service_tests.c src\preview_service.c src\preview_capture.c ^
  src\image_io.c src\atomic_file.c -o build\preview_service_tests.exe ^
  -ldwmapi -lgdi32 -luser32 -lole32 -luuid -lwindowscodecs
if errorlevel 1 exit /b 1
build\preview_service_tests.exe
if errorlevel 1 exit /b 1
