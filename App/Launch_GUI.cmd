@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0GUI.exe" (
    start "" "%~dp0GUI.exe"
    exit /b 0
)
call "%~dp0Launch_Source.cmd"
exit /b %errorlevel%
