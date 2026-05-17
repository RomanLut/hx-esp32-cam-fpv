@echo off
setlocal
powershell -ExecutionPolicy Bypass -File "%~dp0..\scripts\sync_changed_gs_target.ps1" -Target rpi4 -Build -RunAfterBuild %*
set "EXITCODE=%ERRORLEVEL%"
endlocal & exit /b %EXITCODE%
