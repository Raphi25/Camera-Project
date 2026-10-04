@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0BreadboardCameraGUI.exe" (
    start "" "%~dp0BreadboardCameraGUI.exe"
    exit /b 0
)
call "%~dp0Launch_Source.cmd"
exit /b %errorlevel%
