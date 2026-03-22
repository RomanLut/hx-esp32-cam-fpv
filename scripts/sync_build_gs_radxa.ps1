param(
    [string]$RemoteHost = "192.168.3.148",
    [string]$User = "radxa",
    [string]$Password = "radxa",
    [string]$RemoteProjectDir = "/home/radxa/esp32-cam-fpv",
    [string]$RemoteBuildSubdir = "gs",
    [switch]$SkipClean
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$plink = "C:\Program Files\putty\plink.exe"

function Require-Tool([string]$path) {
    if (-not (Test-Path $path)) {
        throw "Required tool not found: $path"
    }
}

Require-Tool $plink

$gitFiles = git -C $repoRoot ls-files --recurse-submodules
if (-not $gitFiles) {
    throw "No tracked files found in $repoRoot"
}

$buildCmd = "cd $RemoteProjectDir/$RemoteBuildSubdir && "
if (-not $SkipClean) {
    $buildCmd += "make clean && "
}
$buildCmd += "make -j4"

Write-Host "Syncing tracked files to ${User}@${RemoteHost}:${RemoteProjectDir} ..."
$gitFiles |
    & tar -cf - -C $repoRoot -T - |
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "cd $RemoteProjectDir && tar -xf -"

Write-Host "Building GS on remote host ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" $buildCmd
