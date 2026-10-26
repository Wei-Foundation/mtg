@echo off
rem Wei compiler build entry point. The real build logic lives in build.ps1.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
