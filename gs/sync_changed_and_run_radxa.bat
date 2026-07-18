@echo off
setlocal
if not defined RADXA_IP_ADDRESS set "RADXA_IP_ADDRESS=192.168.3.39"
powershell -ExecutionPolicy Bypass -File "%~dp0..\scripts\sync_changed_gs_target.ps1" -Target radxa -RemoteHost "%RADXA_IP_ADDRESS%" -Build -RunAfterBuild %*
set "EXITCODE=%ERRORLEVEL%"
endlocal & exit /b %EXITCODE%
