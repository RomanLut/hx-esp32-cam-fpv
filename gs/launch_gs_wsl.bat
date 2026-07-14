@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "GS_DIR=%~dp0"
if "%GS_DIR:~-1%"=="\" set "GS_DIR=%GS_DIR:~0,-1%"
set "WSL_DISTRO=Ubuntu"
set "USBIPD_EXE=C:\Program Files\usbipd-win\usbipd.exe"
set "BUSID="
set "BUSIDS="
set "BUSID_COUNT=0"
set "IFACE="
set "IFACES="
set "IFACE_COUNT=0"
set "TX_IFACE="
set "RX_IFACE_ARGS="
set "GS_WSL_DIR="
set "ATTACHED="
set "IFACE_RETRY_MAX=5"
set "BUSID_FILE=%TEMP%\gs_wsl_busid.txt"
set "BUSIDS_FILE=%TEMP%\gs_wsl_busids.txt"
set "IFACES_FILE=%TEMP%\gs_wsl_ifaces.txt"
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

echo Detecting RTL8812AU/RTL8812EU/RTL8821AU USB adapters...
del /q "%BUSIDS_FILE%" 2>nul
powershell -NoProfile -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$state = & 'C:\Program Files\usbipd-win\usbipd.exe' state | ConvertFrom-Json;" ^
  "$devs = @($state.Devices | Where-Object { $_.BusId -and ($_.InstanceId -match 'VID_0BDA&PID_(8812|881A|A81A|8811|8821|0821)' -or $_.InstanceId -match 'VID_2357&PID_0120' -or $_.Description -match '8812AU|RTL8812AU|88XXAU|RTL88XXAU|8812EU|RTL8812EU|88XXEU|RTL88XXEU|8821AU|RTL8821AU') } | Select-Object -First 2);" ^
  "if ($devs.Count -gt 0) { Set-Content -LiteralPath '%BUSIDS_FILE%' -Value @($devs | ForEach-Object { $_.BusId }) }"
if exist "%BUSIDS_FILE%" (
  for /f "usebackq delims=" %%B in ("%BUSIDS_FILE%") do (
    if not defined BUSIDS (
      set "BUSIDS=%%B"
    ) else (
      set "BUSIDS=!BUSIDS! %%B"
    )
    set /a BUSID_COUNT+=1
  )
)

if not defined BUSIDS (
  echo Could not auto-detect an RTL8812AU, RTL8812EU, or RTL8821AU adapter bus id.
  exit /b 1
)

echo Using %BUSID_COUNT% USB adapters: %BUSIDS%.

echo Starting WSL distro "%WSL_DISTRO%"...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "nohup sleep 300 >/dev/null 2>&1 &" >nul 2>&1

echo Waiting for WSL...
timeout /t 2 /nobreak >nul

for %%B in (%BUSIDS%) do (
  echo Attaching USB adapter %%B to WSL...
  "%USBIPD_EXE%" bind --busid %%B >nul 2>&1
  call :detect_attached "%%B"
  if /i "!ATTACHED!"=="1" (
    echo USB adapter %%B is already attached.
  ) else (
    "%USBIPD_EXE%" attach --wsl %WSL_DISTRO% --busid %%B
    if errorlevel 1 (
      call :detect_attached "%%B"
      if /i not "!ATTACHED!"=="1" (
        echo usbipd attach failed for %%B.
        exit /b 1
      )
    )
  )
)

call :wait_iface_with_restarts
if defined IFACES goto :iface_ready

echo Could not detect %BUSID_COUNT% Wifi interfaces in WSL after %IFACE_RETRY_MAX% retries.
exit /b 1

:iface_ready
call :build_iface_args
echo Using %IFACE_COUNT% interfaces: %IFACES%.
echo Using TX interface %TX_IFACE%.

echo Waiting for Wifi interface readiness in WSL...
for /l %%N in (1,1,60) do (
  set "IFACE_READY="
  set "IFACE_READY=1"
  for %%I in (%IFACES%) do (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" probe_iface_ready "%WSL_DISTRO%" "%%I"
    if errorlevel 1 set "IFACE_READY="
  )
  if /i "!IFACE_READY!"=="1" goto :iface_ready_ok
  timeout /t 1 /nobreak >nul
)

echo Wifi interfaces %IFACES% did not become ready in WSL.
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
wsl.exe -d %WSL_DISTRO% -e bash -lc "cd '%GS_WSL_DIR%' && make -j16"
if errorlevel 1 (
  echo gs build failed.
  exit /b 1
)

if /i "%LAUNCH_MODE%"=="mcp" goto :launch_mcp

echo Launching gs on interfaces %IFACES%...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "cd '%GS_WSL_DIR%' && exec ./gs -rx %RX_IFACE_ARGS% -tx '%TX_IFACE%' -fullscreen 0 -sm 1"
set "EXIT_CODE=%ERRORLEVEL%"

echo gs exited with code %EXIT_CODE%.
exit /b %EXIT_CODE%

:launch_mcp
echo Launching gs in MCP mode on interfaces %IFACES% in a dedicated WSL window...
powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" launch_mcp_full "%WSL_DISTRO%" "%IFACES%" "%GS_WSL_DIR%" "%WSL_MCP_CLIENT%"
if errorlevel 1 (
  echo gs MCP startup failed.
  exit /b 1
)
echo gs MCP ready on interfaces %IFACES%.
exit /b 0

:detect_ifaces
set "IFACES="
set "IFACE_COUNT=0"
del /q "%IFACES_FILE%" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%WSL_HELPER_PS1%" detect_ifaces "%WSL_DISTRO%" "%IFACES_FILE%" "5" "%BUSID_COUNT%"
if exist "%IFACES_FILE%" (
  for /f "usebackq delims=" %%I in ("%IFACES_FILE%") do (
    if not defined IFACES (
      set "IFACES=%%I"
    ) else (
      set "IFACES=!IFACES! %%I"
    )
    set /a IFACE_COUNT+=1
  )
)
:detect_ifaces_done
exit /b 0

:wait_iface_with_restarts
set "IFACES="
for /l %%R in (1,1,%IFACE_RETRY_MAX%) do (
  echo Waiting for %BUSID_COUNT% Wifi interfaces in WSL... attempt %%R/%IFACE_RETRY_MAX%
  call :detect_ifaces
  if defined IFACES (
    if !IFACE_COUNT! geq %BUSID_COUNT% goto :wait_iface_with_restarts_done
  )
  if %%R lss %IFACE_RETRY_MAX% (
    echo Wifi interfaces not found in 5 seconds. Restarting WSL and retrying...
    call :restart_wsl_and_reattach
  )
)
:wait_iface_with_restarts_done
exit /b 0

:restart_wsl_and_reattach
wsl.exe --shutdown >nul 2>&1
timeout /t 1 /nobreak >nul
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "nohup sleep 300 >/dev/null 2>&1 &" >nul 2>&1
timeout /t 2 /nobreak >nul
for %%B in (%BUSIDS%) do (
  "%USBIPD_EXE%" bind --busid %%B >nul 2>&1
  "%USBIPD_EXE%" attach --wsl %WSL_DISTRO% --busid %%B >nul 2>&1
)
exit /b 0

:build_iface_args
set "TX_IFACE="
set "RX_IFACE_ARGS="
for %%I in (%IFACES%) do (
  if not defined TX_IFACE set "TX_IFACE=%%I"
  if not defined RX_IFACE_ARGS (
    set "RX_IFACE_ARGS='%%I'"
  ) else (
    set "RX_IFACE_ARGS=!RX_IFACE_ARGS! '%%I'"
  )
)
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
set "CHECK_BUSID=%~1"
if not defined CHECK_BUSID set "CHECK_BUSID=%BUSID%"
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $state = & 'C:\Program Files\usbipd-win\usbipd.exe' state | ConvertFrom-Json; $dev = $state.Devices | Where-Object { $_.BusId -eq '%CHECK_BUSID%' } | Select-Object -First 1; if ($dev -and $dev.ClientIPAddress) { '1' }"`) do set "ATTACHED=%%I"
exit /b 0
