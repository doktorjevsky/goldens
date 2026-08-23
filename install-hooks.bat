@echo off
setlocal
git config core.hooksPath .githooks
if errorlevel 1 exit /b 1
echo Goldens pre-commit and pre-push test hooks are enabled.
