param(
    [string]$Abi = "arm64-v8a",
    [string]$Config = "Release",
    [string]$MinSdk = "23",
    [string]$OpenCVSource = "$PSScriptRoot\..\..\OpenCV",
    [string]$BuildRoot = "",
    [string]$PrebuiltPlatform = "android/arm64-v8a",
    [string]$ExpectedOpenCVTag = "4.13.0",
    [string]$AndroidSdk = "",
    [string]$AndroidNdk = "",
    [string]$CMakeExe = "",
    [string]$NinjaExe = "",
    [int]$BuildJobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"

function Get-AndroidSdkFromLocalProperties
{
    $localProperties = Resolve-Path "$PSScriptRoot\..\..\..\android_gs\local.properties" -ErrorAction SilentlyContinue
    if(-not $localProperties)
    {
        return ""
    }

    $sdkLine = Get-Content $localProperties | Where-Object { $_ -match "^sdk\.dir=" } | Select-Object -First 1
    if(-not $sdkLine)
    {
        return ""
    }

    return ($sdkLine.Substring("sdk.dir=".Length) -replace "\\:", ":" -replace "\\\\", "\")
}

function Get-LatestDirectory
{
    param([string]$Path)

    if(-not (Test-Path $Path))
    {
        return ""
    }

    $directory = Get-ChildItem $Path -Directory |
        Sort-Object { [version]($_.Name -replace "[^\d\.].*$", "") } -Descending |
        Select-Object -First 1

    if(-not $directory)
    {
        return ""
    }

    return $directory.FullName
}

function Invoke-Checked
{
    param(
        [string]$Program,
        [string[]]$Arguments
    )

    & $Program @Arguments
    if($LASTEXITCODE -ne 0)
    {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

$WrapperRoot = Resolve-Path "$PSScriptRoot\.."

if($AndroidSdk -eq "")
{
    $AndroidSdk = Get-AndroidSdkFromLocalProperties
}

if($AndroidNdk -eq "")
{
    if($env:ANDROID_NDK_HOME)
    {
        $AndroidNdk = $env:ANDROID_NDK_HOME
    }
    elseif($env:ANDROID_NDK_ROOT)
    {
        $AndroidNdk = $env:ANDROID_NDK_ROOT
    }
    elseif($AndroidSdk -ne "")
    {
        $AndroidNdk = Get-LatestDirectory (Join-Path $AndroidSdk "ndk")
    }
}

if($CMakeExe -eq "")
{
    if($AndroidSdk -ne "")
    {
        $cmakeDirectory = Get-LatestDirectory (Join-Path $AndroidSdk "cmake")
        if($cmakeDirectory -ne "")
        {
            $CMakeExe = Join-Path $cmakeDirectory "bin\cmake.exe"
        }
    }
}

if($CMakeExe -eq "")
{
    $CMakeExe = "cmake"
}

if($NinjaExe -eq "")
{
    $candidateNinja = Join-Path (Split-Path $CMakeExe -Parent) "ninja.exe"
    if(Test-Path $candidateNinja)
    {
        $NinjaExe = $candidateNinja
    }
    else
    {
        $NinjaExe = "ninja"
    }
}

if($BuildRoot -eq "")
{
    $BuildRoot = "$PSScriptRoot\..\Build\android-$Abi"
}

if($AndroidNdk -eq "")
{
    throw "Android NDK was not found. Set ANDROID_NDK_HOME or pass -AndroidNdk."
}

$Toolchain = Join-Path $AndroidNdk "build\cmake\android.toolchain.cmake"
if(-not (Test-Path $Toolchain))
{
    throw "Android toolchain file was not found at $Toolchain."
}

$OpenCVInstall = Join-Path $BuildRoot "opencv-install"
$OpenCVBuild = Join-Path $BuildRoot "opencv"
$WrapperBuild = Join-Path $BuildRoot "wrapper"

$OpenCVTag = git -C $OpenCVSource describe --tags --exact-match
if($OpenCVTag -ne $ExpectedOpenCVTag)
{
    throw "OpenCV source must be pinned to tag $ExpectedOpenCVTag, but found '$OpenCVTag'."
}

Write-Host "Building Android OpenCVWrapper for $Abi with NDK $AndroidNdk and $BuildJobs parallel jobs."

$OpenCVConfigureArgs = @(
    "-S", $OpenCVSource,
    "-B", $OpenCVBuild,
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DANDROID_ABI=$Abi",
    "-DANDROID_PLATFORM=android-$MinSdk",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DCMAKE_INSTALL_PREFIX=$OpenCVInstall",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_LIST=core,imgproc,calib3d",
    "-DCPU_DISPATCH=",
    "-DBUILD_TESTS=OFF",
    "-DBUILD_PERF_TESTS=OFF",
    "-DBUILD_EXAMPLES=OFF",
    "-DBUILD_opencv_apps=OFF",
    "-DBUILD_ANDROID_PROJECTS=OFF",
    "-DBUILD_ANDROID_EXAMPLES=OFF",
    "-DBUILD_PROTOBUF=OFF",
    "-DBUILD_JAVA=OFF",
    "-DBUILD_opencv_python2=OFF",
    "-DBUILD_opencv_python3=OFF",
    "-DWITH_AVIF=OFF",
    "-DWITH_IPP=OFF",
    "-DWITH_JASPER=OFF",
    "-DWITH_JPEG=OFF",
    "-DWITH_OPENCL=OFF",
    "-DWITH_OPENEXR=OFF",
    "-DWITH_OPENJPEG=OFF",
    "-DWITH_PNG=OFF",
    "-DWITH_PROTOBUF=OFF",
    "-DWITH_QT=OFF",
    "-DWITH_TIFF=OFF",
    "-DWITH_WEBP=OFF",
    "-DWITH_ZLIB=OFF"
)

Invoke-Checked $CMakeExe $OpenCVConfigureArgs
Invoke-Checked $CMakeExe @("--build", $OpenCVBuild, "--config", $Config, "--target", "install", "--parallel", "$BuildJobs")

$OpenCVConfig = Join-Path $OpenCVBuild "OpenCVConfig.cmake"
if(-not (Test-Path $OpenCVConfig))
{
    throw "OpenCVConfig.cmake was not found under $OpenCVBuild."
}

$WrapperConfigureArgs = @(
    "-S", "$WrapperRoot",
    "-B", "$WrapperBuild",
    "-G", "Ninja",
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DANDROID_ABI=$Abi",
    "-DANDROID_PLATFORM=android-$MinSdk",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DOpenCV_DIR=$OpenCVBuild",
    "-DOPENCV_WRAPPER_PREBUILT_PLATFORM=$PrebuiltPlatform"
)

Invoke-Checked $CMakeExe $WrapperConfigureArgs
Invoke-Checked $CMakeExe @("--build", $WrapperBuild, "--config", $Config, "--target", "install", "--parallel", "$BuildJobs")
