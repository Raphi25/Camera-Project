@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0.venv\Scripts\python.exe" (
    where py.exe >nul 2>nul
    if not errorlevel 1 (
        py -3 -m venv "%~dp0.venv"
    ) else (
        python -m venv "%~dp0.venv"
    )
    if errorlevel 1 goto :failed
)
"%~dp0.venv\Scripts\python.exe" -m pip install -r "%~dp0requirements-gui.txt" pyinstaller
if errorlevel 1 goto :failed
echo Setup complete. Use Launch_Source.cmd or Build_GUI_App.cmd.
pause
exit /b 0
:failed
echo Setup failed. Install Python 3.13 or newer and try again.
pause
exit /b 1
