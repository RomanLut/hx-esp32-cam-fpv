---
name: build-opencv-wrapper
description: Build the repository OpenCV/OpenCVWrapper prebuilts for Windows, Linux via WSL Ubuntu, or Android. Use when asked to build, rebuild, validate, or troubleshoot the GS OpenCV wrapper, OpenCVWrapper, camera calibration wrapper, stabilization wrapper, or OpenCV prebuilt outputs under OpenCV/OpenCVWrapper/Prebuilt.
---

# Build OpenCV Wrapper

## Quick Start

Build from the repository root. The OpenCV source is expected at `OpenCV/OpenCV`, and wrapper output is installed under `OpenCV/OpenCVWrapper/Prebuilt/<platform>`.

Before building, make sure the submodule exists:

```powershell
git submodule update --init OpenCV/OpenCV
```

The OpenCV submodule must be pinned to tag `4.13.0` at commit `fe38fc608f6acb8b68953438a62305d8318f4fcd`. The platform scripts check the exact tag before building; if that check fails, update the submodule checkout instead of bypassing the check.

## Linux / WSL

Always invoke WSL with the Ubuntu distro explicitly:

```powershell
wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"
```

The Linux script defaults to `nproc` parallel jobs. Override it when needed:

```powershell
wsl.exe -d Ubuntu -e bash -lc "cd /mnt/d/Github/esp32-cam-fpv/esp32-cam-fpv && BUILD_JOBS=8 bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"
```

If `cmake` is missing inside Ubuntu, install it with:

```powershell
wsl.exe -d Ubuntu -e bash -lc "sudo apt-get update && sudo apt-get install -y cmake"
```

Expected x64 output:

```text
OpenCV/OpenCVWrapper/Prebuilt/linux/x64/libOpenCVWrapper.so
```

Expected arm64 output when run on arm64 Linux:

```text
OpenCV/OpenCVWrapper/Prebuilt/linux/arm64/libOpenCVWrapper.so
```

## Windows

Run from PowerShell:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_windows.ps1
```

Expected output:

```text
OpenCV/OpenCVWrapper/Prebuilt/windows/x64/OpenCVWrapper.dll
```

## Android

Set the Android NDK path and run:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_android.ps1
```

The script reads `android_gs/local.properties`, uses the latest NDK under that SDK when `ANDROID_NDK_HOME` is not set, and passes the SDK `ninja.exe` explicitly to CMake. Override discovery when needed:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_android.ps1 -AndroidNdk "D:\Android\android-sdk\ndk\26.1.10909125" -BuildJobs 8
```

Default ABI output:

```text
OpenCV/OpenCVWrapper/Prebuilt/android/arm64-v8a/libOpenCVWrapper.so
```

For another ABI:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_android.ps1 -Abi armeabi-v7a -PrebuiltPlatform android/armeabi-v7a
```

## Build Notes

The scripts build OpenCV static modules first, then link them into the wrapper shared library. Current OpenCV module list is:

```text
core
imgproc
calib3d
```

Add `video` to the script `BUILD_LIST` and wrapper `find_package(OpenCV REQUIRED COMPONENTS ...)` when implementing stabilization APIs that use optical flow.

The scripts intentionally disable image codecs, protobuf, IPP, OpenCL, and extra CPU dispatch targets. The wrapper receives decoded GS frames directly, so do not re-enable TIFF/JPEG/WebP/OpenEXR/OpenJPEG/PNG/protobuf unless a wrapper API actually needs those dependencies.

The wrapper CMake configuration uses OpenCV's build-tree `OpenCVConfig.cmake`, not the install-tree package. The install-tree package can reference optional third-party archives that the wrapper does not need.

Do not include OpenCV headers in normal GS code. Keep OpenCV types private to `OpenCV/OpenCVWrapper/src` and expose plain C ABI structs in `OpenCV/OpenCVWrapper/include`.

## Failure Handling

If the build fails, report the first compiler or CMake error clearly and include the platform, command, and missing package/tool. Do not claim the prebuilt exists unless the expected file is present under `OpenCV/OpenCVWrapper/Prebuilt`.
