@echo off
setlocal

if "%~1"=="" (
  echo Usage: %~nx0 ^<QUEST_IP[:PORT]^>
  echo Example: %~nx0 192.168.3.50:5555
  exit /b 1
)

set TARGET=%~1
set PACKAGE=com.esp32camfpv.androidgs
set ACTIVITY=com.esp32camfpv.androidgs/.MainActivity

set ADB=D:\Android\android-sdk\platform-tools\adb.exe
if not exist "%ADB%" set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe

pushd "%~dp0"

"%ADB%" connect %TARGET% >nul

"%ADB%" -s %TARGET% shell am force-stop %PACKAGE%
if errorlevel 1 exit /b 1

call gradlew.bat assembleDebug
if errorlevel 1 exit /b 1

timeout /t 1 /nobreak >nul

rem Reinstall only the APK. Do not clear app data, uninstall, or delete external
rem storage files here; Quest GS recordings are user data and must survive deploys.
"%ADB%" -s %TARGET% install --user 0 -r "app\build\outputs\apk\debug\app-debug.apk"
if errorlevel 1 exit /b 1

"%ADB%" -s %TARGET% shell am start --user 0 -n %ACTIVITY%
if errorlevel 1 exit /b 1

"%ADB%" -s %TARGET% shell pidof %PACKAGE% >nul
if errorlevel 1 exit /b 1

"%ADB%" -s %TARGET% shell dumpsys activity activities | findstr /C:"%ACTIVITY%" | findstr /C:"Resumed" >nul
if errorlevel 1 exit /b 1

echo Oculus Quest GS deployed over Wi-Fi to %TARGET%.
