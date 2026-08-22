@echo off
setlocal

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

if not exist build mkdir build

set "OUTPUT=build\goldens.exe"
if not "%~1"=="" set "OUTPUT=%~1"

%CC% -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter ^
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  -municode -mwindows src\goldens.c src\model.c src\document.c src\window_tracker.c src\image_io.c src\preview_capture.c src\editor_render.c src\ui_layout.c src\ui_tooltip.c src\resource_ops.c -o %OUTPUT% ^
  -lcomctl32 -lole32 -luuid -lwindowscodecs -lshell32 -lshlwapi ^
  -ldwmapi -lgdi32 -lmsimg32 -luser32 -ladvapi32

if errorlevel 1 exit /b 1
echo Built %OUTPUT%
