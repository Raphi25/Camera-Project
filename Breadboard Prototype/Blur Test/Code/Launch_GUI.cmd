@echo off
cd /d "%~dp0"
if exist "%LocalAppData%\Programs\Python\Python313\python.exe" (
    "%LocalAppData%\Programs\Python\Python313\python.exe" gui\GUI.py
) else (
    python gui\GUI.py
)
if errorlevel 1 pause
