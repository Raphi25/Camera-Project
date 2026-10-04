@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0.venv\Scripts\python.exe" (
    echo Run Setup_App.cmd first to install the build dependencies.
    pause
    exit /b 1
)
"%~dp0.venv\Scripts\python.exe" -m PyInstaller --noconfirm --clean --noconsole --onefile ^
  --name BreadboardCameraGUI ^
  --icon "%~dp0Camera.ico" ^
  --add-data "%~dp0Camera.png;." ^
  --collect-all bleak ^
  --collect-all cryptography ^
  --collect-all serial ^
  --distpath "%~dp0." ^
  --workpath "%~dp0build" ^
  --specpath "%~dp0build" ^
  "%~dp0GUI.py"
if errorlevel 1 (
    echo Build failed.
    pause
    exit /b 1
)
echo Built %~dp0BreadboardCameraGUI.exe
pause
exit /b 0
