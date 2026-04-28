# OpenCVWrapper

`OpenCVWrapper` is the GS-facing native wrapper around OpenCV. It keeps OpenCV headers and C++ types out of the main GS code and exports a small C ABI that can be consumed from Windows, Linux, and Android builds.

## Repository Layout

```text
OpenCV/
  OpenCV/                 OpenCV source submodule
  OpenCVWrapper/
    include/              Public wrapper API
    src/                  OpenCV-backed implementation
    scripts/              Platform build scripts
    Prebuilt/             Installed wrapper binaries
```

Initialize the OpenCV source with:

```bash
git submodule update --init OpenCV/OpenCV
```

The OpenCV submodule is pinned to:

```text
OpenCV tag: 4.13.0
OpenCV commit: fe38fc608f6acb8b68953438a62305d8318f4fcd
```

The build scripts check this tag before compiling so release builds do not accidentally use the moving `4.x` branch.

## Static OpenCV Linking

The build scripts compile the required OpenCV modules as static libraries and then link them into the wrapper shared library. The wrapper uses OpenCV's build-tree CMake package because OpenCV's install-tree package can reference optional third-party archives that are not needed by this wrapper. The intended output is a single wrapper library per target platform:

```text
OpenCV/OpenCVWrapper/Prebuilt/windows/x64/OpenCVWrapper.dll
OpenCV/OpenCVWrapper/Prebuilt/linux/x64/libOpenCVWrapper.so
OpenCV/OpenCVWrapper/Prebuilt/linux/arm64/libOpenCVWrapper.so
OpenCV/OpenCVWrapper/Prebuilt/android/arm64-v8a/libOpenCVWrapper.so
```

The wrapper currently builds only the OpenCV modules needed for camera calibration:

```text
core
imgproc
calib3d
```

Image stabilization will likely add `video` later.

The build scripts intentionally disable image codec stacks, protobuf, IPP, OpenCL, and extra CPU dispatch targets. The wrapper receives decoded GS frames directly, so those dependencies are unnecessary for calibration and make WSL builds much slower.

## Build

Windows:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_windows.ps1
```

Linux:

```bash
OpenCV/OpenCVWrapper/scripts/build_linux.sh
```

The Linux script uses `nproc` parallel jobs by default. Override with:

```bash
BUILD_JOBS=8 OpenCV/OpenCVWrapper/scripts/build_linux.sh
```

Android:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_android.ps1
```

The Android script reads `android_gs/local.properties`, uses the latest NDK under that SDK when `ANDROID_NDK_HOME` is not set, and uses the SDK CMake/Ninja tools. Override when needed:

```powershell
.\OpenCV\OpenCVWrapper\scripts\build_android.ps1 -AndroidNdk "D:\Android\android-sdk\ndk\26.1.10909125" -BuildJobs 8
```

## Calibration API

Use `gs_vision_calibrate_camera_from_chessboard_images()` with raw grayscale, BGR, RGB, BGRA, or RGBA images. The chessboard dimensions are the number of inner corners, not the number of printed squares.

OpenCV returns distortion coefficients in this order:

```text
k1, k2, p1, p2, k3
```

The wrapper result exposes them as:

```text
k1, k2, k3, p1, p2
```
