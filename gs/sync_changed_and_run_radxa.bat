@echo off
setlocal
powershell -ExecutionPolicy Bypass -File "%~dp0..\scripts\sync_changed_gs_target.ps1" -Target radxa -Build -RunAfterBuild %*
set "EXITCODE=%ERRORLEVEL%"
endlocal & exit /b %EXITCODE%
