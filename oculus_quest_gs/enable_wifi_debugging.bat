@echo off
setlocal

if "%~1"=="" (
  echo Usage: %~nx0 ^<QUEST_IP^> [PORT]
  echo Example: %~nx0 192.168.3.50 5555
  exit /b 1
)

set QUEST_IP=%~1
set QUEST_PORT=%~2
if "%QUEST_PORT%"=="" set QUEST_PORT=5555

set ADB=D:\Android\android-sdk\platform-tools\adb.exe
if not exist "%ADB%" set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe

"%ADB%" devices
if errorlevel 1 exit /b 1

echo.
echo Switching currently attached USB Quest to TCP/IP on port %QUEST_PORT%...
"%ADB%" tcpip %QUEST_PORT%
if errorlevel 1 (
  echo Failed to enable tcpip mode. Ensure Quest is connected over USB and authorized.
  exit /b 1
)

timeout /t 2 /nobreak >nul

echo Connecting to %QUEST_IP%:%QUEST_PORT%...
"%ADB%" connect %QUEST_IP%:%QUEST_PORT%
if errorlevel 1 exit /b 1

echo.
echo Active devices:
"%ADB%" devices

echo.
echo Wi-Fi debugging ready for %QUEST_IP%:%QUEST_PORT%.
