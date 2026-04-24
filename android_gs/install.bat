@echo off
setlocal

set ADB=D:\Android\android-sdk\platform-tools\adb.exe
if not exist "%ADB%" set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe

"%ADB%" install --user 0 -r "%~dp0app\build\outputs\apk\debug\app-debug.apk"

