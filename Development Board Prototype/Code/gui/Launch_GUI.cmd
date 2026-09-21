@echo off
setlocal

rem Double-click launcher for the Breadboard Prototype GUI.
rem It runs from this folder so relative paths such as gui_password_store.json
rem keep working whether the launcher is started from Explorer or a terminal.

cd /d "%~dp0"

if exist "..\.venv\Scripts\pythonw.exe" (
    start "" "..\.venv\Scripts\pythonw.exe" "%~dp0GUI.py"
    exit /b 0
)

where pythonw.exe >nul 2>nul
if %errorlevel% equ 0 (
    start "" pythonw.exe "%~dp0GUI.py"
    exit /b 0
)

where python.exe >nul 2>nul
if %errorlevel% equ 0 (
    start "" python.exe "%~dp0GUI.py"
    exit /b 0
)

echo Python was not found.
echo Install Python or create a .venv, then run this launcher again.
pause
