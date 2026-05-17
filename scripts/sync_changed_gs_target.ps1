param(
    [ValidateSet("radxa", "rpi2w", "rpi4")]
    [string]$Target,
    [string]$RemoteHost = "",
    [string]$User = "",
    [string]$Password = "",
    [string]$RemoteProjectDir = "",
    [string]$RemoteBuildSubdir = "gs",
    [switch]$Build,
    [switch]$RunAfterBuild
)

$ErrorActionPreference = "Stop"

$plink = "C:\Program Files\putty\plink.exe"

function Require-Tool([string]$path)
{
    if (-not (Test-Path $path))
    {
        throw "Required tool not found: $path"
    }
}

function Invoke-RemoteCommand([string]$description, [string]$command)
{
    if (-not [string]::IsNullOrWhiteSpace($description))
    {
        Write-Host $description
    }

    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" $command
    if ($LASTEXITCODE -ne 0)
    {
        throw "Remote command failed with exit code ${LASTEXITCODE}: $command"
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

function Test-RsyncHasChanges([object[]]$RsyncOutput)
{
    foreach ($lineObj in $RsyncOutput)
    {
        $line = [string]$lineObj
        if ([string]::IsNullOrWhiteSpace($line))
        {
            continue
        }

        if ($line -match '^(<|>|\*deleting )')
        {
            return $true
        }
    }

    return $false
}

$targetDefaults = @{
    "radxa" = @{
        RemoteHost = "192.168.3.148"
        User = "radxa"
        Password = "radxa"
        RemoteProjectDir = "/home/radxa/esp32-cam-fpv"
    }
    "rpi4" = @{
        RemoteHost = "192.168.3.147"
        User = "pi"
        Password = "1234"
        RemoteProjectDir = "/home/pi/esp32-cam-fpv"
    }
    "rpi2w" = @{
        RemoteHost = ""
        User = "pi"
        Password = "raspberry"
        RemoteProjectDir = "/home/pi/esp32-cam-fpv"
    }
}

if (-not $targetDefaults.ContainsKey($Target))
{
    throw "Unsupported target: $Target"
}

$selected = $targetDefaults[$Target]
if ([string]::IsNullOrWhiteSpace($RemoteHost)) { $RemoteHost = $selected.RemoteHost }
if ([string]::IsNullOrWhiteSpace($User)) { $User = $selected.User }
if ([string]::IsNullOrWhiteSpace($Password)) { $Password = $selected.Password }
if ([string]::IsNullOrWhiteSpace($RemoteProjectDir)) { $RemoteProjectDir = $selected.RemoteProjectDir }
if ([string]::IsNullOrWhiteSpace($RemoteHost))
{
    throw "RemoteHost is required for target '$Target'. Pass -RemoteHost <ip-or-hostname>."
}

Require-Tool $plink

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
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
        "--exclude=*.avi"
        "--exclude=*.mjpeg"
        "--exclude=recordings/"
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
Invoke-RemoteCommand "Preparing remote directories on ${User}@${RemoteHost}:${RemoteProjectDir} ..." $mkdirCommand

$repoRootForWsl = Convert-ToWslPath $repoRoot
if ([string]::IsNullOrWhiteSpace($repoRootForWsl))
{
    throw "Failed to resolve WSL path for repo root: $repoRoot"
}

Write-Host "Target: $Target"
Write-Host "Remote: ${User}@${RemoteHost}:${RemoteProjectDir}"

Write-Host "Syncing OpenCV trees via rsync ..."
$opencvWrapperHadRsyncChanges = $false
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
                    "--itemize-changes " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    $rsyncOutput = & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand 2>&1
    if ($rsyncOutput) { $rsyncOutput | ForEach-Object { Write-Host $_ } }
    if ($LASTEXITCODE -ne 0) { throw "rsync failed for path: $relativePath" }
    if ($relativePath -eq "OpenCV/OpenCVWrapper" -and (Test-RsyncHasChanges $rsyncOutput))
    {
        $opencvWrapperHadRsyncChanges = $true
    }
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
                    "--itemize-changes " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    $rsyncOutput = & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand 2>&1
    if ($rsyncOutput) { $rsyncOutput | ForEach-Object { Write-Host $_ } }
    if ($LASTEXITCODE -ne 0) { throw "rsync failed for path: $relativePath" }
}

$remoteScriptsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/scripts"
$remoteGsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/gs"
$remoteGsDirUnquoted = "$RemoteProjectDir/gs"
Write-Host "Normalizing line endings on remote scripts ..."
Invoke-RemoteCommand "" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} + && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} +"
Write-Host "Restoring executable flags on remote scripts ..."
Invoke-RemoteCommand "" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} + && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} +"

if ($Build)
{
    if ($opencvWrapperHadRsyncChanges)
    {
        Invoke-RemoteCommand "Building OpenCV wrapper on remote host ..." "cd $RemoteProjectDir && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"
    }
    else
    {
        Write-Host "Skipping OpenCV wrapper rebuild: rsync reported no OpenCV/OpenCVWrapper changes."
    }

    Write-Host "Building GS on remote host ..."
    Invoke-RemoteCommand "" "cd $RemoteProjectDir/$RemoteBuildSubdir && make -j4"

    if ($RunAfterBuild)
    {
        Write-Host "Launching GS via launch.sh on remote host ..."
        $launchCmd = "cd $RemoteProjectDir/gs && chmod +x ./launch.sh && " +
                     "if command -v tmux >/dev/null 2>&1; then " +
                     "  (tmux kill-session -t gslaunch 2>/dev/null || true) && " +
                     "  tmux new-session -d -s gslaunch ./launch.sh && " +
                     "  tmux list-sessions; " +
                     "else " +
                     "  nohup ./launch.sh >/tmp/gs-launch.log 2>&1 < /dev/null & " +
                     "fi; " +
                     "echo GS_LAUNCH_TRIGGERED; exit 0"
        Invoke-RemoteCommand "" $launchCmd
    }
}
