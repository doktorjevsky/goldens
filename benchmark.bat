@echo off
setlocal

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

if not exist build mkdir build

%CC% -std=c17 -O2 -Wall -Wextra tools\render_benchmark.c ^
  src\editor_render.c -o build\render_benchmark.exe ^
  -lgdi32 -lmsimg32 -luser32
if errorlevel 1 exit /b 1

build\render_benchmark.exe
