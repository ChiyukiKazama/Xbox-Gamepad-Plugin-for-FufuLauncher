@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set "taskExitCode=%ERRORLEVEL%"
if not "%taskExitCode%"=="0" echo Build failed. See the error above.
if "%taskExitCode%"=="0" echo Build completed.
pause
exit /b %taskExitCode%
