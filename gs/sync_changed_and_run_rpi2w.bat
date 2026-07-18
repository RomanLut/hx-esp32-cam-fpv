@echo off
setlocal
if not defined RPI2W_IP_ADDRESS set "RPI2W_IP_ADDRESS=192.168.3.39"
if not defined RPI2W_PASSWORD set "RPI2W_PASSWORD=1234"
powershell -ExecutionPolicy Bypass -File "%~dp0..\scripts\sync_changed_gs_target.ps1" -Target rpi2w -RemoteHost "%RPI2W_IP_ADDRESS%" -Password "%RPI2W_PASSWORD%" -Build -RunAfterBuild %*
set "EXITCODE=%ERRORLEVEL%"
endlocal & exit /b %EXITCODE%
