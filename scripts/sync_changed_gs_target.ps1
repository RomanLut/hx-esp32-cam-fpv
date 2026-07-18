param(
    [ValidateSet("radxa", "rpi2w", "rpi4")]
    [string]$Target,
    [string]$RemoteHost = "",
    [string]$User = "",
    [string]$Password = "",
    [string]$HostKey = "",
    [string]$RemoteProjectDir = "",
    [string]$RemoteBuildSubdir = "gs",
    [switch]$Build,
    [switch]$RunAfterBuild
)

$ErrorActionPreference = "Stop"

$plink = "C:\Program Files\putty\plink.exe"

#===================================================================================
#===================================================================================
# Verifies that a required local executable is available.
function Require-Tool([string]$path)
{
    if (-not (Test-Path $path))
    {
        throw "Required tool not found: $path"
    }
}

#===================================================================================
#===================================================================================
# Runs a command on the selected target and treats any nonzero result as an error.
function Invoke-RemoteCommand([string]$description, [string]$command)
{
    if (-not [string]::IsNullOrWhiteSpace($description))
    {
        Write-Host $description
    }

    & $plink @plinkConnectionArgs $command
    if ($LASTEXITCODE -ne 0)
    {
        throw "Remote command failed with exit code ${LASTEXITCODE}: $command"
    }
}

#===================================================================================
#===================================================================================
# Quotes a value so it can be safely embedded in a Bash command.
function Convert-ToBashSingleQuoted([string]$value)
{
    return "'" + ($value -replace "'", "'\''") + "'"
}

#===================================================================================
#===================================================================================
# Converts an absolute Windows path to the equivalent WSL mount path.
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

#===================================================================================
#===================================================================================
# Reports whether rsync transferred or deleted at least one filesystem entry.
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

#===================================================================================
#===================================================================================
# Reports whether rsync changed wrapper sources, headers, or CMake build inputs.
function Test-RsyncHasOpenCvWrapperBuildChanges([object[]]$RsyncOutput)
{
    foreach ($lineObj in $RsyncOutput)
    {
        $line = [string]$lineObj
        if ($line -match '(?:^|\s)(?:src/|include/|CMakeLists\.txt$|OPENCV_VERSION\.txt$)')
        {
            return $true
        }
    }

    return $false
}

#===================================================================================
#===================================================================================
# Installs the WSL packages required by the changed-file sync when they are missing.
function Initialize-WslSyncTools()
{
    $toolCheckCommand = "command -v rsync >/dev/null 2>&1 && command -v sshpass >/dev/null 2>&1 && command -v ssh-keyscan >/dev/null 2>&1 && command -v ssh-keygen >/dev/null 2>&1"
    & wsl.exe -d Ubuntu -u root -- bash -lc $toolCheckCommand
    if ($LASTEXITCODE -eq 0)
    {
        return
    }

    Write-Host "Installing missing WSL sync tools (rsync, sshpass, openssh-client) ..."
    $installCommand = "export DEBIAN_FRONTEND=noninteractive; apt-get update && apt-get install -y rsync sshpass openssh-client"
    & wsl.exe -d Ubuntu -u root -- bash -lc $installCommand
    if ($LASTEXITCODE -ne 0)
    {
        throw "Failed to install the required WSL sync tools."
    }
}

#===================================================================================
#===================================================================================
# Reads the key currently presented by the target so stale caches never block deploys.
function Resolve-CurrentSshHostKey()
{
    $scanCommand = "ssh-keyscan -T 5 -t ed25519 $RemoteHost 2>/dev/null | ssh-keygen -lf - -E sha256"
    $scanOutput = & wsl.exe -d Ubuntu -u root -- bash -lc $scanCommand 2>&1
    if ($LASTEXITCODE -ne 0)
    {
        if ($scanOutput)
        {
            $scanOutput | ForEach-Object { Write-Host $_ }
        }
        throw "Could not read the current SSH host key from $RemoteHost."
    }

    $fingerprintMatch = [regex]::Match(($scanOutput -join "`n"), 'SHA256:[A-Za-z0-9+/]+')
    if (-not $fingerprintMatch.Success)
    {
        throw "ssh-keyscan returned no usable ED25519 fingerprint for $RemoteHost."
    }

    return $fingerprintMatch.Value
}

#===================================================================================
#===================================================================================
# Installs rsync on the target when the remote half of changed-file sync is missing.
function Initialize-RemoteSyncTools()
{
    Invoke-RemoteCommand "Enabling remote NTP time synchronization ..." "sudo timedatectl set-ntp true"

    # Fresh Radxa images can boot with a 2017/2023 clock and no active NTP. HTTPS apt
    # then rejects valid repositories, so seed UTC from the deployment host first.
    $hostUtc = [DateTime]::UtcNow.ToString("yyyy-MM-dd HH:mm:ss")
    $installCommand = "if ! command -v rsync >/dev/null 2>&1; then " +
                      "echo 'Installing rsync on the remote target ...'; " +
                      "sudo date -u -s '$hostUtc' >/dev/null && " +
                      "(sudo env DEBIAN_FRONTEND=noninteractive apt-get update || true) && " +
                      "sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y rsync; " +
                      "fi; command -v rsync >/dev/null 2>&1"
    Invoke-RemoteCommand "Checking remote rsync availability ..." $installCommand
}

#===================================================================================
#===================================================================================
# Uses make's dependency graph to determine whether the remote GS needs rebuilding.
function Test-RemoteGsNeedsBuild()
{
    Write-Host "Checking whether the remote GS build is current ..."
    $makeQueryOutput = & $plink @plinkConnectionArgs "cd $RemoteProjectDir/$RemoteBuildSubdir && make --no-print-directory -q" 2>&1
    $makeQueryExitCode = $LASTEXITCODE
    if ($makeQueryOutput)
    {
        $makeQueryOutput | ForEach-Object { Write-Host $_ }
    }
    if ($makeQueryExitCode -eq 0)
    {
        return $false
    }
    if ($makeQueryExitCode -eq 1)
    {
        return $true
    }

    throw "Remote make dependency check failed with exit code ${makeQueryExitCode}."
}

#===================================================================================
#===================================================================================
# Reports whether a GS process is currently running on the remote target.
function Test-RemoteGsIsRunning()
{
    $processQueryOutput = & $plink @plinkConnectionArgs "pgrep -x gs >/dev/null 2>&1" 2>&1
    $processQueryExitCode = $LASTEXITCODE
    if ($processQueryOutput)
    {
        $processQueryOutput | ForEach-Object { Write-Host $_ }
    }
    if ($processQueryExitCode -eq 0)
    {
        return $true
    }
    if ($processQueryExitCode -eq 1)
    {
        return $false
    }

    throw "Remote GS process check failed with exit code ${processQueryExitCode}."
}

$targetDefaults = @{
    "radxa" = @{
        RemoteHost = "192.168.3.39"
        User = "radxa"
        Password = "radxa"
        HostKey = ""
        RemoteProjectDir = "/home/radxa/esp32-cam-fpv"
    }
    "rpi4" = @{
        RemoteHost = "192.168.3.147"
        User = "pi"
        Password = "1234"
        HostKey = ""
        RemoteProjectDir = "/home/pi/esp32-cam-fpv"
    }
    "rpi2w" = @{
        RemoteHost = ""
        User = "pi"
        Password = "raspberry"
        HostKey = ""
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
if ([string]::IsNullOrWhiteSpace($HostKey)) { $HostKey = $selected.HostKey }
if ([string]::IsNullOrWhiteSpace($RemoteProjectDir)) { $RemoteProjectDir = $selected.RemoteProjectDir }
if ([string]::IsNullOrWhiteSpace($RemoteHost))
{
    throw "RemoteHost is required for target '$Target'. Pass -RemoteHost <ip-or-hostname>."
}

Require-Tool $plink
Initialize-WslSyncTools

if ([string]::IsNullOrWhiteSpace($HostKey))
{
    $HostKey = Resolve-CurrentSshHostKey
    Write-Host "Using the SSH key currently presented by ${RemoteHost}: $HostKey"
}

$plinkConnectionArgs = @("-ssh", "-batch", "-no-antispoof", "-pw", $Password)
if (-not [string]::IsNullOrWhiteSpace($HostKey))
{
    $plinkConnectionArgs += @("-hostkey", $HostKey)
}
$plinkConnectionArgs += "${User}@${RemoteHost}"

Initialize-RemoteSyncTools

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
$opencvWrapperBuildInputsChanged = $false
$gsRuntimeHadRsyncChanges = $false
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
    $excludeArgs += @("--exclude=.git", "--exclude=.git/", "--exclude=.cache/", "--exclude=.gradle/")
    $excludeArgsString = if ($excludeArgs.Count -gt 0) { ($excludeArgs -join " ") + " " } else { "" }
    $rsyncCommand = "export SSHPASS=$passwordForBash; " +
                    "export RSYNC_RSH='ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR'; " +
                    "sshpass -e rsync -az --delete --omit-dir-times --no-perms --no-owner --no-group " +
                    "--itemize-changes " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    $rsyncOutput = & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand 2>&1
    if ($rsyncOutput) { $rsyncOutput | ForEach-Object { Write-Host $_ } }
    if ($LASTEXITCODE -ne 0) { throw "rsync failed for path: $relativePath" }
    if ($relativePath -eq "OpenCV/OpenCVWrapper" -and (Test-RsyncHasOpenCvWrapperBuildChanges $rsyncOutput))
    {
        $opencvWrapperBuildInputsChanged = $true
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
    $excludeArgs += @("--exclude=.git", "--exclude=.git/", "--exclude=.cache/", "--exclude=.gradle/")
    $excludeArgsString = if ($excludeArgs.Count -gt 0) { ($excludeArgs -join " ") + " " } else { "" }
    $rsyncCommand = "export SSHPASS=$passwordForBash; " +
                    "export RSYNC_RSH='ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR'; " +
                    "sshpass -e rsync -az --delete --omit-dir-times --no-perms --no-owner --no-group " +
                    "--itemize-changes " +
                    $excludeArgsString +
                    "$sourceForBash $destinationForBash"
    $rsyncOutput = & wsl.exe -d Ubuntu -u root -- bash -lc $rsyncCommand 2>&1
    if ($rsyncOutput) { $rsyncOutput | ForEach-Object { Write-Host $_ } }
    if ($LASTEXITCODE -ne 0) { throw "rsync failed for path: $relativePath" }
    if (Test-RsyncHasChanges $rsyncOutput)
    {
        if ($relativePath -ne "scripts")
        {
            $gsRuntimeHadRsyncChanges = $true
        }
    }
}

$remoteScriptsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/scripts"
$remoteGsDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/gs"
$remoteGsDirUnquoted = "$RemoteProjectDir/gs"
$remoteWrapperDir = Convert-ToBashSingleQuoted "$RemoteProjectDir/OpenCV/OpenCVWrapper"
Write-Host "Normalizing line endings on remote scripts ..."
# Rewriting an already-LF file changes its timestamp and makes rsync transfer it again next time.
# Use byte values instead of shell \r quoting; some target sed variants interpret \r as
# the letter r and silently corrupt lines ending in r. Files are written only when needed.
$normalizeLfCode = "import pathlib,sys`nfor name in sys.argv[1:]:`n p=pathlib.Path(name); data=p.read_bytes()`n if bytes([13,10]) in data: p.write_bytes(data.replace(bytes([13,10]),bytes([10])))"
$normalizeLfCodeBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($normalizeLfCode))
$normalizeLfCodeByteList = (([Text.Encoding]::ASCII.GetBytes($normalizeLfCodeBase64)) -join ",")
$normalizeLfExec = "-exec python3 -c 'import base64;exec(base64.b64decode(bytes([$normalizeLfCodeByteList])))' {} +"
Invoke-RemoteCommand "" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) $normalizeLfExec && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) $normalizeLfExec && find $remoteWrapperDir \( -path '*/Build' -o -path '*/Prebuilt' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) $normalizeLfExec"
Write-Host "Restoring executable flags on remote scripts ..."
Invoke-RemoteCommand "" "find $remoteScriptsDir -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} + && find $remoteGsDir \( -path '$remoteGsDirUnquoted/build' -o -path '$remoteGsDirUnquoted/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} + && find $remoteWrapperDir \( -path '*/Build' -o -path '*/Prebuilt' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} +"

if ($Build)
{
    $gsNeedsBuild = Test-RemoteGsNeedsBuild
    $gsIsRunning = Test-RemoteGsIsRunning
    $compileNeeded = $opencvWrapperBuildInputsChanged -or $gsNeedsBuild
    $restartNeeded = $gsRuntimeHadRsyncChanges -or $compileNeeded -or (-not $gsIsRunning)
    $stopNeeded = $compileNeeded -or ($RunAfterBuild -and $restartNeeded)

    if ($stopNeeded)
    {
        # GS normally runs as root through launch.sh. Stop it before replacing build outputs,
        # and wait for termination so the compiler never competes with GS for Radxa resources.
        $stopGsCmd = "(tmux kill-session -t gslaunch 2>/dev/null || true) && " +
                     "(sudo pkill -TERM -x gs 2>/dev/null || true); " +
                     "attempt=0; while pgrep -x gs >/dev/null 2>&1 && [ `$attempt -lt 50 ]; do sleep 0.1; attempt=`$((attempt + 1)); done; " +
                     "if pgrep -x gs >/dev/null 2>&1; then sudo pkill -KILL -x gs 2>/dev/null || true; sleep 0.2; fi; " +
                     "if pgrep -x gs >/dev/null 2>&1; then echo 'Failed to stop GS.' >&2; exit 1; fi"
        Invoke-RemoteCommand "Stopping the running GS before build/restart ..." $stopGsCmd
    }

    if ($opencvWrapperBuildInputsChanged)
    {
        Invoke-RemoteCommand "Building OpenCV wrapper on remote host ..." "cd $RemoteProjectDir && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"
    }
    else
    {
        Write-Host "Skipping OpenCV wrapper rebuild: rsync reported no wrapper source, header, or CMake input changes."
    }

    if ($gsNeedsBuild)
    {
        Write-Host "Building only out-of-date GS targets on remote host ..."
        Invoke-RemoteCommand "" "cd $RemoteProjectDir/$RemoteBuildSubdir && make --no-print-directory -j`$(nproc)"
    }
    else
    {
        Write-Host "Skipping GS compilation: make reports that all targets are current."
    }

    if ($RunAfterBuild -and $restartNeeded)
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
    elseif ($RunAfterBuild)
    {
        Write-Host "No files changed, no build is required, and GS is already running; leaving it untouched."
    }
}
