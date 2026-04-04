param(
    [string]$RemoteHost = "192.168.3.148",
    [string]$User = "radxa",
    [string]$Password = "radxa",
    [string]$RemoteProjectDir = "/home/radxa/esp32-cam-fpv",
    [string]$RemoteBuildSubdir = "gs",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$plink = "C:\Program Files\putty\plink.exe"
$pscp = "C:\Program Files\putty\pscp.exe"

function Require-Tool([string]$path)
{
    if (-not (Test-Path $path)) {
        throw "Required tool not found: $path"
    }
}

Require-Tool $plink
Require-Tool $pscp

$changedFiles = @(git -C $repoRoot diff --name-only -- `
    gs `
    components_gs `
    components/common `
    assets_gs)

if ($changedFiles.Count -eq 0) {
    Write-Host "No changed GS files to sync."
} else {
    Write-Host "Syncing changed GS files to ${User}@${RemoteHost}:${RemoteProjectDir} ..."
    foreach ($relativePath in $changedFiles) {
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }

        $normalizedPath = $relativePath -replace "\\", "/"
        $localPath = Join-Path $repoRoot $relativePath
        if (-not (Test-Path -LiteralPath $localPath)) {
            continue
        }

        $remoteDir = [System.IO.Path]::GetDirectoryName($normalizedPath)
        if ([string]::IsNullOrWhiteSpace($remoteDir)) {
            $remoteDir = "."
        } else {
            $remoteDir = $remoteDir -replace "\\", "/"
        }

        & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "mkdir -p '$RemoteProjectDir/$remoteDir'"
        & $pscp -scp -batch -pw $Password $localPath "${User}@${RemoteHost}:${RemoteProjectDir}/${remoteDir}/"
    }
}

if ($Build) {
    Write-Host "Building GS on remote host ..."
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "cd $RemoteProjectDir/$RemoteBuildSubdir && make -j4"
}
