param(
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64",
    [string]$Config = "Release",
    [string]$OpenCVSource = "$PSScriptRoot\..\..\OpenCV",
    [string]$BuildRoot = "$PSScriptRoot\..\Build\windows-x64",
    [string]$PrebuiltPlatform = "windows/x64",
    [string]$ExpectedOpenCVTag = "4.13.0"
)

$ErrorActionPreference = "Stop"

$WrapperRoot = Resolve-Path "$PSScriptRoot\.."
$OpenCVInstall = Join-Path $BuildRoot "opencv-install"
$OpenCVBuild = Join-Path $BuildRoot "opencv"
$WrapperBuild = Join-Path $BuildRoot "wrapper"

$OpenCVTag = git -C $OpenCVSource describe --tags --exact-match
if($OpenCVTag -ne $ExpectedOpenCVTag)
{
    throw "OpenCV source must be pinned to tag $ExpectedOpenCVTag, but found '$OpenCVTag'."
}

cmake -S $OpenCVSource -B $OpenCVBuild -G $Generator -A $Arch `
    -DCMAKE_INSTALL_PREFIX=$OpenCVInstall `
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON `
    -DBUILD_SHARED_LIBS=OFF `
    "-DBUILD_LIST=core,imgproc,calib3d" `
    "-DCPU_DISPATCH=" `
    -DBUILD_TESTS=OFF `
    -DBUILD_PERF_TESTS=OFF `
    -DBUILD_EXAMPLES=OFF `
    -DBUILD_opencv_apps=OFF `
    -DBUILD_PROTOBUF=OFF `
    -DBUILD_JAVA=OFF `
    -DBUILD_opencv_python2=OFF `
    -DBUILD_opencv_python3=OFF `
    -DWITH_1394=OFF `
    -DWITH_AVIF=OFF `
    -DWITH_FFMPEG=OFF `
    -DWITH_GSTREAMER=OFF `
    -DWITH_IPP=OFF `
    -DWITH_JASPER=OFF `
    -DWITH_JPEG=OFF `
    -DWITH_OPENCL=OFF `
    -DWITH_OPENEXR=OFF `
    -DWITH_OPENJPEG=OFF `
    -DWITH_PNG=OFF `
    -DWITH_PROTOBUF=OFF `
    -DWITH_QT=OFF `
    -DWITH_TIFF=OFF `
    -DWITH_WEBP=OFF `
    -DWITH_ZLIB=OFF

cmake --build $OpenCVBuild --config $Config --target install

$OpenCVConfig = Join-Path $OpenCVBuild "OpenCVConfig.cmake"
if(-not (Test-Path $OpenCVConfig))
{
    throw "OpenCVConfig.cmake was not found under $OpenCVBuild."
}

cmake -S $WrapperRoot -B $WrapperBuild -G $Generator -A $Arch `
    -DOpenCV_DIR="$OpenCVBuild" `
    -DOPENCV_WRAPPER_PREBUILT_PLATFORM=$PrebuiltPlatform

cmake --build $WrapperBuild --config $Config --target install
