@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "GS_DIR=%~dp0"
if "%GS_DIR:~-1%"=="\" set "GS_DIR=%GS_DIR:~0,-1%"
set "WSL_DISTRO=Ubuntu"
set "USBIPD_EXE=C:\Program Files\usbipd-win\usbipd.exe"
set "BUSID="
set "IFACE="
set "GS_WSL_DIR="
set "ATTACHED="
set "BUSID_FILE=%TEMP%\gs_wsl_busid.txt"
set "WSL_HELPER_PS1=%GS_DIR%\launch_gs_wsl_helper.ps1"
set "LAUNCH_MODE=normal"
set "WSL_MCP_CLIENT="

if /i "%~1"=="-mcp" set "LAUNCH_MODE=mcp"
if /i "%~1"=="-mcp-stop" set "LAUNCH_MODE=mcp-stop"

if not exist "%USBIPD_EXE%" (
  echo usbipd-win not found at "%USBIPD_EXE%".
  exit /b 1
)

if not exist "%GS_DIR%\Makefile" (
  echo gs Makefile not found in "%GS_DIR%".
  exit /b 1
)

if not exist "%WSL_HELPER_PS1%" (
  echo WSL helper script not found at "%WSL_HELPER_PS1%".
  exit /b 1
)

if /i "%LAUNCH_MODE%"=="mcp-stop" (
  echo Stopping WSL gs MCP instance...
  powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" stop_mcp "%WSL_DISTRO%"
  echo Stopped WSL gs MCP instance.
  exit /b 0
)

echo Detecting RTL8812AU USB adapter...
del /q "%BUSID_FILE%" 2>nul
powershell -NoProfile -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$state = & 'C:\Program Files\usbipd-win\usbipd.exe' state | ConvertFrom-Json;" ^
  "$dev = $state.Devices | Where-Object { $_.InstanceId -match 'VID_0BDA&PID_8812' -or $_.Description -match '8812AU|RTL8812AU' } | Select-Object -First 1;" ^
  "if ($dev) { Set-Content -LiteralPath '%BUSID_FILE%' -Value $dev.BusId -NoNewline }"
if exist "%BUSID_FILE%" set /p "BUSID="<"%BUSID_FILE%"

if not defined BUSID (
  echo Could not auto-detect RTL8812AU adapter bus id.
  exit /b 1
)

echo Using BUSID %BUSID%.

echo Starting WSL distro "%WSL_DISTRO%"...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "nohup sleep 300 >/dev/null 2>&1 &" >nul 2>&1

echo Waiting for WSL...
timeout /t 2 /nobreak >nul

echo Attaching USB adapter %BUSID% to WSL...
"%USBIPD_EXE%" bind --busid %BUSID% >nul 2>&1
call :detect_attached
if /i "%ATTACHED%"=="1" (
  echo USB adapter is already attached.
) else (
  "%USBIPD_EXE%" attach --wsl %WSL_DISTRO% --busid %BUSID%
  if errorlevel 1 (
    call :detect_attached
    if /i not "%ATTACHED%"=="1" (
      echo usbipd attach failed.
      exit /b 1
    )
  )
)

echo Waiting for Wifi interface in WSL...
for /l %%N in (1,1,15) do (
  call :detect_iface
  if defined IFACE goto :iface_ready
  timeout /t 1 /nobreak >nul
)

echo Could not detect Wifi interface in WSL after USB attach.
exit /b 1

:iface_ready
echo Using interface %IFACE%.

echo Waiting for Wifi interface readiness in WSL...
for /l %%N in (1,1,60) do (
  call :probe_iface_ready
  if /i "!IFACE_READY!"=="1" goto :iface_ready_ok
  timeout /t 1 /nobreak >nul
)

echo Wifi interface %IFACE% did not become ready in WSL.
exit /b 1

:iface_ready_ok

echo Resolving WSL path...
call :resolve_wsl_dir
if not defined GS_WSL_DIR (
  echo Could not resolve gs path inside WSL.
  exit /b 1
)

set "WSL_MCP_CLIENT=%GS_WSL_DIR%/scripts/gs_mcp_client.py"

echo Building gs under WSL...
wsl.exe -d %WSL_DISTRO% -e bash -lc "cd '%GS_WSL_DIR%' && make -j2"
if errorlevel 1 (
  echo gs build failed.
  exit /b 1
)

if /i "%LAUNCH_MODE%"=="mcp" goto :launch_mcp

echo Launching gs on interface %IFACE%...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "cd '%GS_WSL_DIR%' && exec ./gs -rx '%IFACE%' -tx '%IFACE%' -fullscreen 0 -sm 1"
set "EXIT_CODE=%ERRORLEVEL%"

echo gs exited with code %EXIT_CODE%.
exit /b %EXIT_CODE%

:launch_mcp
echo Launching gs in MCP mode on interface %IFACE% in a dedicated WSL window...
powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" launch_mcp_full "%WSL_DISTRO%" "%IFACE%" "%GS_WSL_DIR%" "%WSL_MCP_CLIENT%"
if errorlevel 1 (
  echo gs MCP startup failed.
  exit /b 1
)
echo gs MCP ready on interface %IFACE%.
exit /b 0

:detect_iface
set "IFACE="
del /q "%BUSID_FILE%" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" detect_iface "%WSL_DISTRO%" "%BUSID_FILE%"
if exist "%BUSID_FILE%" set /p "IFACE="<"%BUSID_FILE%"
:detect_iface_done
exit /b 0

:probe_iface_ready
set "IFACE_READY="
powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" probe_iface_ready "%WSL_DISTRO%" "%IFACE%"
if not errorlevel 1 set "IFACE_READY=1"
exit /b 0

:resolve_wsl_dir
set "GS_WSL_DIR="
set "GS_WSL_DIR=%GS_DIR:\=/%"
if "%GS_WSL_DIR:~1,1%"==":" (
  set "WSL_DRIVE=!GS_WSL_DIR:~0,1!"
  if /i "!WSL_DRIVE!"=="A" set "WSL_DRIVE=a"
  if /i "!WSL_DRIVE!"=="B" set "WSL_DRIVE=b"
  if /i "!WSL_DRIVE!"=="C" set "WSL_DRIVE=c"
  if /i "!WSL_DRIVE!"=="D" set "WSL_DRIVE=d"
  if /i "!WSL_DRIVE!"=="E" set "WSL_DRIVE=e"
  if /i "!WSL_DRIVE!"=="F" set "WSL_DRIVE=f"
  if /i "!WSL_DRIVE!"=="G" set "WSL_DRIVE=g"
  if /i "!WSL_DRIVE!"=="H" set "WSL_DRIVE=h"
  if /i "!WSL_DRIVE!"=="I" set "WSL_DRIVE=i"
  if /i "!WSL_DRIVE!"=="J" set "WSL_DRIVE=j"
  if /i "!WSL_DRIVE!"=="K" set "WSL_DRIVE=k"
  if /i "!WSL_DRIVE!"=="L" set "WSL_DRIVE=l"
  if /i "!WSL_DRIVE!"=="M" set "WSL_DRIVE=m"
  if /i "!WSL_DRIVE!"=="N" set "WSL_DRIVE=n"
  if /i "!WSL_DRIVE!"=="O" set "WSL_DRIVE=o"
  if /i "!WSL_DRIVE!"=="P" set "WSL_DRIVE=p"
  if /i "!WSL_DRIVE!"=="Q" set "WSL_DRIVE=q"
  if /i "!WSL_DRIVE!"=="R" set "WSL_DRIVE=r"
  if /i "!WSL_DRIVE!"=="S" set "WSL_DRIVE=s"
  if /i "!WSL_DRIVE!"=="T" set "WSL_DRIVE=t"
  if /i "!WSL_DRIVE!"=="U" set "WSL_DRIVE=u"
  if /i "!WSL_DRIVE!"=="V" set "WSL_DRIVE=v"
  if /i "!WSL_DRIVE!"=="W" set "WSL_DRIVE=w"
  if /i "!WSL_DRIVE!"=="X" set "WSL_DRIVE=x"
  if /i "!WSL_DRIVE!"=="Y" set "WSL_DRIVE=y"
  if /i "!WSL_DRIVE!"=="Z" set "WSL_DRIVE=z"
  set "GS_WSL_DIR=/mnt/!WSL_DRIVE!!GS_WSL_DIR:~2!"
)
exit /b 0

:detect_attached
set "ATTACHED="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $state = & 'C:\Program Files\usbipd-win\usbipd.exe' state | ConvertFrom-Json; $dev = $state.Devices | Where-Object { $_.BusId -eq '%BUSID%' } | Select-Object -First 1; if ($dev -and $dev.ClientIPAddress) { '1' }"`) do set "ATTACHED=%%I"
exit /b 0
