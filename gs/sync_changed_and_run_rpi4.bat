@echo off
setlocal
if not defined RPI4_IP_ADDRESS set "RPI4_IP_ADDRESS=192.168.3.147"
powershell -ExecutionPolicy Bypass -File "%~dp0..\scripts\sync_changed_gs_target.ps1" -Target rpi4 -RemoteHost "%RPI4_IP_ADDRESS%" -Build -RunAfterBuild %*
set "EXITCODE=%ERRORLEVEL%"
endlocal & exit /b %EXITCODE%
