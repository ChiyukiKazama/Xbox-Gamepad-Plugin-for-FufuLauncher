@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish.ps1" %*
set "taskExitCode=%ERRORLEVEL%"
if not "%taskExitCode%"=="0" echo Publish failed. See the error above.
if "%taskExitCode%"=="0" echo Release package created in bin\publish.
pause
exit /b %taskExitCode%
