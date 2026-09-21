@echo off
setlocal

rem Optional packager for the Breadboard Prototype GUI.
rem This creates ..\App\BreadboardCameraGUI.exe using PyInstaller.

cd /d "%~dp0"

set "PY=python.exe"
if exist "..\.venv\Scripts\python.exe" set "PY=..\.venv\Scripts\python.exe"

"%PY%" -c "import PyInstaller" >nul 2>nul
if errorlevel 1 (
    echo PyInstaller is not installed for this Python environment.
    echo.
    echo To install it, run:
    echo   "%PY%" -m pip install pyinstaller
    echo.
    pause
    exit /b 1
)

"%PY%" -m PyInstaller --noconfirm --clean --noconsole --onefile ^
  --name BreadboardCameraGUI ^
  --icon "%~dp0..\App\Camera.ico" ^
  --add-data "%~dp0..\App\Camera.png;." ^
  --collect-all bleak ^
  --collect-all cryptography ^
  --collect-all serial ^
  --distpath "%~dp0..\App" ^
  --workpath "%~dp0build" ^
  --specpath "%~dp0build" ^
  "%~dp0GUI.py"
if errorlevel 1 (
    echo.
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo Built app:
echo   %~dp0..\App\BreadboardCameraGUI.exe
echo.
pause
