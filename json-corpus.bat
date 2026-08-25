@echo off
setlocal

if "%~1"=="" (
  echo Usage: json-corpus.bat C:\path\to\JSONTestSuite
  exit /b 2
)

set "CC=x86_64-w64-mingw32-clang.exe"
where %CC% >nul 2>nul
if errorlevel 1 set "CC=C:\tools\llvm-mingw-20260616-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

if not exist build mkdir build
%CC% -std=c17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DCOBJMACROS -municode ^
  tests\json_corpus_tests.c src\model.c src\document.c ^
  -o build\json_corpus_tests.exe -luser32
if errorlevel 1 exit /b 1

build\json_corpus_tests.exe "%~1\test_parsing"
