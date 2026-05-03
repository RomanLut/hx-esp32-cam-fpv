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
    "OpenCV",
    "gs",
    "components_gs",
    "components/common",
    "assets_gs",
    "scripts"
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

# Windows tar / SCP strips +x; rsync uses --no-perms. Normalize LF and chmod for scripts/ and gs/ (e.g. launch.sh).
$remoteScriptsDir = "$RemoteProjectDir/scripts"
$remoteGsDir = "$RemoteProjectDir/gs"
$normalizeShellArtifacts = @(
    "find '$remoteScriptsDir' -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} +",
    "find '$remoteScriptsDir' -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} +",
    "find '$remoteGsDir' \( -path '$remoteGsDir/build' -o -path '$remoteGsDir/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r`$//' {} +",
    "find '$remoteGsDir' \( -path '$remoteGsDir/build' -o -path '$remoteGsDir/.vscode' \) -prune -o -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod +x {} +"
) -join " && "

Write-Host "Syncing GS runtime tree to ${User}@${RemoteHost}:${RemoteProjectDir} ..."
if (Test-Path $localArchivePath) {
    Remove-Item -LiteralPath $localArchivePath -Force
}

Push-Location $repoRoot
try {
    & $tar -cf $localArchivePath `
        --exclude=OpenCV/OpenCVWrapper/Build `
        --exclude=OpenCV/OpenCVWrapper/Prebuilt `
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
    & $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "mkdir -p $RemoteProjectDir && find $RemoteProjectDir -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} + && cd $RemoteProjectDir && tar -xf $remoteArchivePath && rm -f $remoteArchivePath && $normalizeShellArtifacts"
}
finally {
    if (Test-Path $localArchivePath) {
        Remove-Item -LiteralPath $localArchivePath -Force
    }
}

Write-Host "Building OpenCV wrapper on remote host ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" "cd $RemoteProjectDir && bash OpenCV/OpenCVWrapper/scripts/build_linux.sh"

Write-Host "Building GS on remote host ..."
& $plink -ssh -batch -no-antispoof -pw $Password "${User}@${RemoteHost}" $buildCmd
