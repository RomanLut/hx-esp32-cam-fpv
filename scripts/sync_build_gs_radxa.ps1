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
$pscp = "C:\Program Files\putty\pscp.exe"
$tar = (Get-Command tar.exe -ErrorAction Stop).Source
$localArchivePath = Join-Path ([System.IO.Path]::GetTempPath()) "esp32-cam-fpv-radxa-sync.tar"
$remoteArchivePath = "/tmp/esp32-cam-fpv-radxa-sync.tar"
$syncPaths = @(
    "gs",
    "components_gs",
    "components/common",
    "assets_gs"
)

function Require-Tool([string]$path) {
    if (-not (Test-Path $path)) {
        throw "Required tool not found: $path"
    }
}

Require-Tool $plink
Require-Tool $pscp
Require-Tool $tar

$buildCmd = "cd $RemoteProjectDir/$RemoteBuildSubdir && "
if (-not $SkipClean) {
    $buildCmd += "make clean && "
}
$buildCmd += "make -j4"

Write-Host "Syncing GS runtime tree to ${User}@${RemoteHost}:${RemoteProjectDir} ..."
if (Test-Path $localArchivePath) {
    Remove-Item -LiteralPath $localArchivePath -Force
}

Push-Location $repoRoot
try {
    & $tar -cf $localArchivePath `
        --exclude=gs/build `
        --exclude=gs/.vscode `
        --exclude=gs/gs `
        @syncPaths
}
finally {
    Pop-Location
}

if (-not (Test-Path $localArchivePath)) {
    throw "Failed to create sync archive: $localArchivePath"
}

try {
    & $pscp -scp -batch -pw $Password $localArchivePath "${User}@${RemoteHost}:${remoteArchivePath}"
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "mkdir -p $RemoteProjectDir && find $RemoteProjectDir -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} + && cd $RemoteProjectDir && tar -xf $remoteArchivePath && rm -f $remoteArchivePath"
}
finally {
    if (Test-Path $localArchivePath) {
        Remove-Item -LiteralPath $localArchivePath -Force
    }
}

Write-Host "Building GS on remote host ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" $buildCmd
