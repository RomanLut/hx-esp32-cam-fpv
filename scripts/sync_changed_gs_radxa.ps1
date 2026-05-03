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

function Require-Tool([string]$path)
{
    if (-not (Test-Path $path))
    {
        throw "Required tool not found: $path"
    }
}

function Convert-ToBashSingleQuoted([string]$value)
{
    return "'" + ($value -replace "'", "'\''") + "'"
}

function Convert-ToWslPath([string]$windowsPath)
{
    $normalized = $windowsPath -replace "\\", "/"
    if ($normalized.Length -lt 3 -or $normalized[1] -ne ":")
    {
        throw "Unsupported Windows path for WSL conversion: $windowsPath"
    }

    $drive = $normalized.Substring(0, 1).ToLowerInvariant()
    $suffix = $normalized.Substring(2)
    return "/mnt/$drive$suffix"
}

Require-Tool $plink

$opencvSyncPaths = @(
    "OpenCV/OpenCV",
    "OpenCV/OpenCVWrapper"
)

$gsSyncPaths = @(
    "gs",
    "components_gs",
    "components/common",
    "assets_gs",
    "scripts"
)

$rsyncExcludesByPath = @{
    "OpenCV/OpenCVWrapper" = @(
        "--exclude=Build/"
        "--exclude=Prebuilt/"
    )
    "gs" = @(
        "--exclude=build/"
        "--exclude=gs"
        "--exclude=gs.ini"
        "--exclude=imgui.ini"
    )
}

$remoteDirs = @(
    $RemoteProjectDir,
    "$RemoteProjectDir/OpenCV",
    "$RemoteProjectDir/OpenCV/OpenCV",
    "$RemoteProjectDir/OpenCV/OpenCVWrapper",
    "$RemoteProjectDir/gs",
    "$RemoteProjectDir/components_gs",
    "$RemoteProjectDir/components",
    "$RemoteProjectDir/components/common",
    "$RemoteProjectDir/assets_gs",
    "$RemoteProjectDir/scripts"
)

$mkdirArgs = ($remoteDirs | ForEach-Object { Convert-ToBashSingleQuoted $_ }) -join " "
$mkdirCommand = "mkdir -p $mkdirArgs"

Write-Host "Preparing remote directories on ${User}@${RemoteHost}:${RemoteProjectDir} ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" $mkdirCommand

$repoRootForWsl = Convert-ToWslPath $repoRoot
if ([string]::IsNullOrWhiteSpace($repoRootForWsl))
{
    throw "Failed to resolve WSL path for repo root: $repoRoot"
}

Write-Host "Syncing OpenCV trees via rsync ..."
foreach ($relativePath in $opencvSyncPaths)
{
    $sourceForWsl = Convert-ToWslPath (Join-Path $repoRoot $relativePath)
    $sourceForBash = Convert-ToBashSingleQuoted ($sourceForWsl.TrimEnd("/") + "/")
    $destinationForBash = Convert-ToBashSingleQuoted ("${User}@${RemoteHost}:${RemoteProjectDir}/$relativePath/")
    $passwordForBash = Convert-ToBashSingleQuoted $Password
    $excludeArgs = @()
    if ($rsyncExcludesByPath.ContainsKey($relativePath))
    {
        $excludeArgs = $rsyncExcludesByPath[$relativePath]
    }
    $excludeArgsString = if ($excludeArgs.Count -gt 0) { ($excludeArgs -join " ") + " " } else { "" }
    $rsyncCommand = "export SSHPASS=$passwordForBash; " +
                    "export RSYNC_RSH='ssh -o StrictHostKeyChecking=no'; " +
                    "sshpass -e rsync -az --delete --omit-dir-times --no-perms --no-owner --no-group " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand
}

Write-Host "Syncing GS runtime trees via rsync ..."
foreach ($relativePath in $gsSyncPaths)
{
    $sourceForWsl = Convert-ToWslPath (Join-Path $repoRoot $relativePath)
    $sourceForBash = Convert-ToBashSingleQuoted ($sourceForWsl.TrimEnd("/") + "/")
    $destinationForBash = Convert-ToBashSingleQuoted ("${User}@${RemoteHost}:${RemoteProjectDir}/$relativePath/")
    $passwordForBash = Convert-ToBashSingleQuoted $Password
    $excludeArgs = @()
    if ($rsyncExcludesByPath.ContainsKey($relativePath))
    {
        $excludeArgs = $rsyncExcludesByPath[$relativePath]
    }
    $excludeArgsString = if ($excludeArgs.Count -gt 0) { ($excludeArgs -join " ") + " " } else { "" }
    $rsyncCommand = "export SSHPASS=$passwordForBash; " +
                    "export RSYNC_RSH='ssh -o StrictHostKeyChecking=no'; " +
                    "sshpass -e rsync -az --delete --omit-dir-times --no-perms --no-owner --no-group " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand
}

$remoteScriptsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/scripts"
$remoteGsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/gs"
$remoteGsDirUnquoted = "$RemoteProjectDir/gs"
Write-Host "Normalizing line endings on remote scripts ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} + && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} +"

Write-Host "Restoring executable flags on remote scripts ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} + && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} +"

if ($Build)
{
    Write-Host "Building OpenCV wrapper on remote host ..."
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "cd $RemoteProjectDir && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"

    Write-Host "Building GS on remote host ..."
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "cd $RemoteProjectDir/$RemoteBuildSubdir && make -j4"
}
