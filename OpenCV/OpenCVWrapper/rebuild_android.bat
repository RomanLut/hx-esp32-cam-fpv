@echo off
setlocal
rem Rebuild OpenCVWrapper Android prebuilt using scripts\build_android.ps1 (NDK + SDK cmake from android_gs\local.properties).
rem Optional args are passed through, e.g. rebuild_android.bat -BuildJobs 8 -Abi armeabi-v7a -PrebuiltPlatform android/armeabi-v7a

cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_android.ps1" %*
exit /b %ERRORLEVEL%
