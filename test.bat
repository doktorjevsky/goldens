@echo off
setlocal

pushd "%~dp0"

call build.bat build\goldens-test.exe
if errorlevel 1 (
  popd
  exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\run-test-suites.ps1
set "TEST_RESULT=%errorlevel%"

popd
exit /b %TEST_RESULT%
