@echo off
setlocal
rem Rebuild OpenCVWrapper Linux prebuilt inside WSL Ubuntu from this repo checkout.
rem Requires: WSL distro "Ubuntu", curl, and a C++ toolchain (build-essential). If cmake is missing,
rem build_linux.sh downloads a portable Kitware CMake to /tmp (see OPENCV_WRAPPER_BOOTSTRAP_CMAKE in that script).

cd /d "%~dp0\..\.."
set "REPO_WIN=%CD%"

for /f "usebackq delims=" %%I in (`wsl.exe -d Ubuntu -e wslpath -a "%REPO_WIN%"`) do set "REPO_WSL=%%I"
if not defined REPO_WSL (
    echo Failed to resolve WSL path for "%REPO_WIN%". Is Ubuntu installed? >&2
    exit /b 1
)

echo Repo Windows: %REPO_WIN%
echo Repo WSL:     %REPO_WSL%
wsl.exe -d Ubuntu -e bash -lc "cd '%REPO_WSL%' && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"
exit /b %ERRORLEVEL%
