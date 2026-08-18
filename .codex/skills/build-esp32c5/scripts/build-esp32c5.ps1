param
(
    [ValidateSet("prepare", "build", "clean", "upload", "package")]
    [string]$Action = "prepare",
    [string]$Port,
    [switch]$OpenExplorer
)

$ErrorActionPreference = "Stop"

$RepositoryRoot = "D:\Github\esp32-cam-fpv\esp32-cam-fpv"
$ProjectDirectory = "Q:\air_firmware_esp32c5"
$PlatformIoRoot = "C:\Users\roman\.platformio"
$PlatformIoExe = "C:\Users\roman\.platformio\penv\Scripts\pio.exe"
$PlatformIoPython = "C:\Users\roman\.platformio\penv\Scripts\python.exe"
$PackagesDirectory = "P:\packages"
$FrameworkDirectory = Join-Path $PackagesDirectory "framework-espidf"
$RequiredFrameworkVersion = "3.50505"

function Set-ShortDrive
{
    param
    (
        [string]$DriveLetter,
        [string]$Target
    )

    $mappingPattern = "^$([Regex]::Escape($DriveLetter)):\\: => (.+)$"
    $mapping = subst | Where-Object { $_ -match $mappingPattern } | Select-Object -First 1

    if ($mapping)
    {
        $currentTarget = ([Regex]::Match($mapping, $mappingPattern)).Groups[1].Value.Trim()
        if (-not $currentTarget.Equals($Target, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "$DriveLetter`: is already mapped to '$currentTarget'; expected '$Target'."
        }
        return
    }

    & subst "$DriveLetter`:" $Target
    if ($LASTEXITCODE -ne 0)
    {
        throw "Failed to map $DriveLetter`: to '$Target'."
    }
}

function Get-PackageVersion
{
    param([string]$Directory)

    $manifest = Join-Path $Directory "package.json"
    if (-not (Test-Path -LiteralPath $manifest))
    {
        return $null
    }

    return (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).version
}

function Select-RequiredFramework
{
    $currentVersion = Get-PackageVersion $FrameworkDirectory
    if ($currentVersion -and $currentVersion.StartsWith($RequiredFrameworkVersion, [StringComparison]::Ordinal))
    {
        return
    }

    $candidates = @(
        Get-ChildItem -LiteralPath $PackagesDirectory -Directory -Filter "framework-espidf@*" |
            Where-Object {
                $version = Get-PackageVersion $_.FullName
                $version -and $version.StartsWith($RequiredFrameworkVersion, [StringComparison]::Ordinal)
            }
    )

    if ($candidates.Count -ne 1)
    {
        throw "Expected exactly one installed ESP-IDF $RequiredFrameworkVersion package, found $($candidates.Count)."
    }

    if (Test-Path -LiteralPath $FrameworkDirectory)
    {
        $backupName = "framework-espidf@version-$currentVersion"
        $backupPath = Join-Path $PackagesDirectory $backupName
        if (Test-Path -LiteralPath $backupPath)
        {
            throw "Cannot preserve the current framework because '$backupPath' already exists."
        }
        Move-Item -LiteralPath $FrameworkDirectory -Destination $backupPath
    }

    Move-Item -LiteralPath $candidates[0].FullName -Destination $FrameworkDirectory
}

function Assert-NoFirmwareBuild
{
    $busy = @(
        Get-CimInstance Win32_Process |
            Where-Object {
                $_.Name -match "^(cmake|ninja|riscv32-esp-elf-gcc|riscv32-esp-elf-g\+\+)\.exe$" -or
                ($_.Name -eq "python.exe" -and $_.CommandLine -match "platformio.*\brun\b")
            }
    )

    if ($busy.Count -gt 0)
    {
        $details = ($busy | ForEach-Object { "$($_.ProcessId): $($_.Name) $($_.CommandLine)" }) -join [Environment]::NewLine
        throw "Another firmware build is active. Wait for it to finish before continuing:$([Environment]::NewLine)$details"
    }
}

function Initialize-BuildEnvironment
{
    foreach ($requiredPath in @($RepositoryRoot, $PlatformIoRoot, $PlatformIoExe, $PlatformIoPython))
    {
        if (-not (Test-Path -LiteralPath $requiredPath))
        {
            throw "Required path does not exist: $requiredPath"
        }
    }

    Set-ShortDrive -DriveLetter "P" -Target $PlatformIoRoot
    Set-ShortDrive -DriveLetter "Q" -Target $RepositoryRoot
    Select-RequiredFramework

    $selectedVersion = Get-PackageVersion $FrameworkDirectory
    Write-Output "Prepared Q: repository mapping and P: PlatformIO mapping."
    Write-Output "Selected framework-espidf $selectedVersion at $FrameworkDirectory."
}

function Get-UploadPort
{
    if ($Port)
    {
        $present = Get-CimInstance Win32_SerialPort | Where-Object { $_.DeviceID -eq $Port }
        if (-not $present)
        {
            throw "Requested port '$Port' is not presently enumerated."
        }
        return $Port
    }

    $ports = @(
        Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match "VID_303A" -and $_.FriendlyName -match "\((COM\d+)\)" } |
            ForEach-Object { ([Regex]::Match($_.FriendlyName, "\((COM\d+)\)")).Groups[1].Value }
    )

    if ($ports.Count -eq 0)
    {
        throw "No present Espressif USB port was found. Put the C5 in download mode (hold BOOT, tap RESET, release BOOT)."
    }
    if ($ports.Count -gt 1)
    {
        throw "Multiple Espressif ports are present ($($ports -join ', ')); rerun with -Port COMx."
    }
    return $ports[0]
}

Initialize-BuildEnvironment

if ($Action -eq "prepare")
{
    exit 0
}

if ($Action -in @("build", "clean", "upload"))
{
    Assert-NoFirmwareBuild
    $env:PLATFORMIO_CORE_DIR = "P:\"
}

if ($Action -eq "build")
{
    & $PlatformIoExe run -e esp32c5 -d $ProjectDirectory
    exit $LASTEXITCODE
}

if ($Action -eq "clean")
{
    & $PlatformIoExe run -e esp32c5 -d $ProjectDirectory -t clean
    exit $LASTEXITCODE
}

if ($Action -eq "upload")
{
    $uploadPort = Get-UploadPort
    Write-Output "Uploading ESP32-C5 on $uploadPort."

    # The user has already forced ROM download mode. Do not let PlatformIO's default-reset
    # sequence disturb that state, and force UTF-8 so esptool's progress bar works on CP1251.
    $env:PYTHONUTF8 = "1"
    $env:PYTHONIOENCODING = "utf-8"
    $buildDirectory = Join-Path $ProjectDirectory ".pio\build\esp32c5"
    $flashImages = @(
        (Join-Path $buildDirectory "bootloader.bin"),
        (Join-Path $buildDirectory "partitions.bin"),
        (Join-Path $buildDirectory "ota_data_initial.bin"),
        (Join-Path $buildDirectory "firmware.bin")
    )
    foreach ($image in $flashImages)
    {
        if (-not (Test-Path -LiteralPath $image))
        {
            throw "Build firmware first; missing '$image'."
        }
    }

    & $PlatformIoPython -m esptool `
        --chip esp32c5 `
        --port $uploadPort `
        --baud 460800 `
        --before no-reset `
        --after hard-reset `
        write-flash `
        -z `
        --flash-mode dio `
        --flash-freq 80m `
        --flash-size detect `
        0x2000 $flashImages[0] `
        0x8000 $flashImages[1] `
        0xe000 $flashImages[2] `
        0x10000 $flashImages[3]
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }

    Start-Sleep -Seconds 3
    $portStillPresent = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -eq $uploadPort }
    if (-not $portStillPresent)
    {
        Write-Output "$uploadPort disappeared after reset; the application took ownership of the USB pins."
        exit 0
    }

    # A hard reset can leave ESP32-C5 in DOWNLOAD(UART0/USB). The watchdog reset must be
    # requested from the ROM loader without a flasher stub or it has no effect.
    Write-Output "$uploadPort remains in the ROM loader; requesting the C5 watchdog reset."
    & $PlatformIoPython -m esptool `
        --chip esp32c5 `
        --port $uploadPort `
        --no-stub `
        --before no-reset `
        --after watchdog-reset `
        chip-id
    if ($LASTEXITCODE -ne 0)
    {
        exit $LASTEXITCODE
    }

    Start-Sleep -Seconds 3
    $portStillPresent = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -eq $uploadPort }
    if ($portStillPresent)
    {
        throw "$uploadPort remained present after the watchdog reset; application startup is not verified."
    }

    Write-Output "$uploadPort disappeared after the watchdog reset; application startup is verified."
    exit 0
}

$firmware = Join-Path $ProjectDirectory ".pio\build\esp32c5\firmware.bin"
if (-not (Test-Path -LiteralPath $firmware))
{
    throw "Build firmware first; missing '$firmware'."
}

$packetsText = Get-Content -LiteralPath "Q:\components\common\packets.h" -Raw
$fecText = Get-Content -LiteralPath "Q:\components\common\fec.h" -Raw
$firmwareVersion = ([Regex]::Match($packetsText, '#define\s+FW_VERSION\s+"([^"]+)"')).Groups[1].Value
$packetVersion = ([Regex]::Match($fecText, '#define\s+PACKET_VERSION\s+(\d+)')).Groups[1].Value
if (-not $firmwareVersion -or -not $packetVersion)
{
    throw "Could not derive FW_VERSION and PACKET_VERSION."
}

$outputDirectory = "Q:\firmware_artifacts\air_firmware_esp32c5"
$packageExitCode = 0
Push-Location "Q:\"
try
{
    & $PlatformIoPython ".github\scripts\package_firmware.py" `
        --project "air_firmware_esp32c5" `
        --version "$firmwareVersion.$packetVersion" `
        --output-dir $outputDirectory
    $packageExitCode = $LASTEXITCODE
}
finally
{
    Pop-Location
}
if ($packageExitCode -ne 0)
{
    exit $packageExitCode
}

$otaPath = Join-Path $outputDirectory "air_firmware_esp32c5_ota.bin"
$otaHash = (Get-FileHash -LiteralPath $otaPath -Algorithm SHA256).Hash
Write-Output "OTA image: $otaPath"
Write-Output "OTA SHA-256: $otaHash"

if ($OpenExplorer)
{
    $realOtaPath = Join-Path $RepositoryRoot "firmware_artifacts\air_firmware_esp32c5\air_firmware_esp32c5_ota.bin"
    Start-Process explorer.exe -ArgumentList "/select,`"$realOtaPath`""
}
