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
set "IFACE_FILE=%TEMP%\gs_wsl_iface.txt"
set "GS_WSL_DIR_FILE=%TEMP%\gs_wsl_dir.txt"

if not exist "%USBIPD_EXE%" (
  echo usbipd-win not found at "%USBIPD_EXE%".
  exit /b 1
)

if not exist "%GS_DIR%\Makefile" (
  echo gs Makefile not found in "%GS_DIR%".
  exit /b 1
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

echo Resolving WSL path...
call :resolve_wsl_dir
if not defined GS_WSL_DIR (
  echo Could not resolve gs path inside WSL.
  exit /b 1
)

echo Building gs under WSL...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "cd '%GS_WSL_DIR%' && make -j4"
if errorlevel 1 (
  echo gs build failed.
  exit /b 1
)

echo Launching gs on interface %IFACE%...
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "cd '%GS_WSL_DIR%' && exec ./gs -rx '%IFACE%' -tx '%IFACE%' -fullscreen 0 -sm 1"
set "EXIT_CODE=%ERRORLEVEL%"

echo gs exited with code %EXIT_CODE%.
exit /b %EXIT_CODE%

:detect_iface
set "IFACE="
del /q "%IFACE_FILE%" 2>nul
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "ls /sys/class/net 2>/dev/null | grep -E '^(wlx|wlan)' | head -n1" > "%IFACE_FILE%"
if exist "%IFACE_FILE%" set /p "IFACE="<"%IFACE_FILE%"
exit /b 0

:resolve_wsl_dir
set "GS_WSL_DIR="
del /q "%GS_WSL_DIR_FILE%" 2>nul
wsl.exe -d %WSL_DISTRO% -u root -- bash -lc "wslpath -a '%GS_DIR%'" > "%GS_WSL_DIR_FILE%"
if exist "%GS_WSL_DIR_FILE%" set /p "GS_WSL_DIR="<"%GS_WSL_DIR_FILE%"
exit /b 0

:detect_attached
set "ATTACHED="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $state = & 'C:\Program Files\usbipd-win\usbipd.exe' state | ConvertFrom-Json; $dev = $state.Devices | Where-Object { $_.BusId -eq '%BUSID%' } | Select-Object -First 1; if ($dev -and $dev.ClientIPAddress) { '1' }"`) do set "ATTACHED=%%I"
exit /b 0
