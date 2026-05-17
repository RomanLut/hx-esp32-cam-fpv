@echo off
setlocal

set ADB=D:\Android\android-sdk\platform-tools\adb.exe
if not exist "%ADB%" set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe

rem Reinstall only the APK. Do not clear app data, uninstall, or delete external
rem storage files here; Quest GS recordings are user data and must survive installs.
"%ADB%" install --user 0 -r "%~dp0app\build\outputs\apk\debug\app-debug.apk"
