@echo off
setlocal

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

set "RC=x86_64-w64-mingw32-windres.exe"
where %RC% >nul 2>nul
if errorlevel 1 set "RC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-windres.exe"

if not exist build mkdir build

set "OUTPUT=build\goldens.exe"
if not "%~1"=="" set "OUTPUT=%~1"
set "WARNINGS=-Wall -Wextra -Wconversion -Wsign-conversion -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror -Wno-unused-parameter"

%RC% -I src -I assets -O coff src\goldens.rc -o build\goldens-res.o
if errorlevel 1 exit /b 1

%CC% -std=c17 -O2 %WARNINGS% ^
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  -municode -mwindows src\goldens.c src\model.c src\document.c src\image_io.c src\clipboard_image.c src\capture_bundle.c src\window_capture.c src\editor_render.c src\ui_layout.c src\ui_tooltip.c src\ui_tool_icon.c src\resource_tree.c src\resource_ops.c src\atomic_file.c src\history.c src\resource_watcher.c build\goldens-res.o -o %OUTPUT% ^
  -lcomctl32 -lole32 -luuid -lwindowscodecs -lshell32 -lshlwapi ^
  -ldwmapi -lgdi32 -lmsimg32 -luser32 -ladvapi32

if errorlevel 1 exit /b 1
echo Built %OUTPUT%
