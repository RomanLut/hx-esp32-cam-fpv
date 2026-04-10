param(
    [Parameter(Mandatory = $true)]
    [string]$Action,

    [string]$Distro,

    [string]$Arg1,

    [string]$Arg2,

    [string]$Arg3
)

$ErrorActionPreference = 'Stop'

function Invoke-WslCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DistroName,

        [Parameter(Mandatory = $true)]
        [string]$CommandText
    )

    # Keep WSL calls inside a normal PowerShell process. Direct wsl.exe calls from
    # the batch launcher were tripping "Input redirection is not supported" in this
    # automation host even when the same command worked interactively.
    $escaped = $CommandText.Replace('"', '\"')
    $standardOutput = & wsl.exe -d $DistroName -u root -- bash -lc "exec 0</dev/null; $escaped" 2>&1
    $combinedOutput = @($standardOutput) -join [Environment]::NewLine

    return [PSCustomObject]@{
        ExitCode = $LASTEXITCODE
        StdOut = $combinedOutput
        StdErr = ''
    }
}

function Escape-BashSingleQuotedText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    return $Value.Replace("'", "'\''")
}

function Get-WslGsMatchPattern {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Interface
    )

    return "^./gs -rx $Interface -tx $Interface -fullscreen 0 -sm 1`$"
}

function Stop-WslGsInstance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DistroName,

        [Parameter(Mandatory = $true)]
        [string]$MatchPattern,

        [string]$PidPath = "/tmp/gs-wsl-mcp.pid"
    )

    # Do not use pkill -f with the full GS command text here. The helper itself runs
    # a bash -lc command containing the same pattern, so pkill would match its shell.
    $stopCommand = "pgrep -f '$MatchPattern' | xargs -r kill >/dev/null 2>&1 || true; rm -f '$PidPath' /tmp/gs-wsl-mcp.log"
    return Invoke-WslCommand -DistroName $DistroName -CommandText $stopCommand
}

function Start-WslGsMcpInstance {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DistroName,

        [Parameter(Mandatory = $true)]
        [string]$GsDir,

        [Parameter(Mandatory = $true)]
        [string]$Interface
    )

    # Detached WSL launches were unstable in testing: GS could bring up MCP and then
    # disappear immediately. Keep GS in its own visible WSL console so SDL/WSLg stay
    # attached to an interactive session, while the helper polls MCP separately.
    $gsCommand = "cd '$GsDir' && exec ./gs -rx '$Interface' -tx '$Interface' -fullscreen 0 -sm 1"
    $cmdLine = "/c start """" wsl.exe -d $DistroName -u root -- bash -lc ""$gsCommand"""
    $process = Start-Process -FilePath 'cmd.exe' -ArgumentList $cmdLine -PassThru

    return [PSCustomObject]@{
        ExitCode = $(if ($null -eq $process) { 1 } else { 0 })
        StdOut = ''
        StdErr = ''
    }
}

function Get-WslPrimaryIp {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DistroName
    )

    $ipResult = Invoke-WslCommand -DistroName $DistroName -CommandText "hostname -I"
    if ($ipResult.ExitCode -ne 0) {
        return $null
    }

    $tokens = $ipResult.StdOut.Trim().Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($tokens.Count -eq 0) {
        return $null
    }

    return $tokens[0]
}

function Remove-WindowsMcpProxy {
    param(
        [int]$Port = 17654
    )

    & netsh interface portproxy delete v4tov4 listenaddress=127.0.0.1 listenport=$Port | Out-Null
}

function Ensure-WindowsMcpProxy {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConnectAddress,

        [int]$Port = 17654
    )

    & sc.exe start iphlpsvc | Out-Null
    Remove-WindowsMcpProxy -Port $Port
    & netsh interface portproxy add v4tov4 listenaddress=127.0.0.1 listenport=$Port connectaddress=$ConnectAddress connectport=$Port | Out-Null
    return ($LASTEXITCODE -eq 0)
}

switch ($Action) {
    'detect_iface' {
        if ([string]::IsNullOrWhiteSpace($Arg1)) {
            throw 'detect_iface requires an output file path.'
        }

        $result = Invoke-WslCommand -DistroName $Distro -CommandText "ls /sys/class/net 2>/dev/null | grep -E '^(wlx|wlan)' | head -n1"
        $iface = $result.StdOut.Trim()
        if (-not [string]::IsNullOrWhiteSpace($iface)) {
            Set-Content -LiteralPath $Arg1 -Value $iface -NoNewline
        }

        exit $result.ExitCode
    }

    'probe_iface_ready' {
        if ([string]::IsNullOrWhiteSpace($Arg1)) {
            throw 'probe_iface_ready requires an interface name.'
        }

        $iface = $Arg1.Replace("'", "'\''")
        $commandText = "ip link show dev '$iface' >/dev/null 2>&1 && " +
            "iw dev '$iface' info >/dev/null 2>&1 && " +
            "ip link set dev '$iface' down >/dev/null 2>&1 && " +
            "iw dev '$iface' set type managed >/dev/null 2>&1 && " +
            "ip link set dev '$iface' up >/dev/null 2>&1"

        $result = Invoke-WslCommand -DistroName $Distro -CommandText $commandText
        exit $result.ExitCode
    }

    'launch_mcp_full' {
        if ([string]::IsNullOrWhiteSpace($Arg1) -or
            [string]::IsNullOrWhiteSpace($Arg2) -or
            [string]::IsNullOrWhiteSpace($Arg3)) {
            throw 'launch_mcp_full requires interface, WSL gs dir, and WSL client path.'
        }

        $iface = Escape-BashSingleQuotedText $Arg1
        $gsDir = Escape-BashSingleQuotedText $Arg2
        $clientPath = Escape-BashSingleQuotedText $Arg3
        $pidPath = "/tmp/gs-wsl-mcp.pid"
        $matchPattern = Get-WslGsMatchPattern -Interface $iface

        $cleanupResult = Stop-WslGsInstance -DistroName $Distro -MatchPattern $matchPattern -PidPath $pidPath
        if ($cleanupResult.ExitCode -ne 0) {
            exit $cleanupResult.ExitCode
        }

        $launchResult = Start-WslGsMcpInstance -DistroName $Distro -GsDir $gsDir -Interface $iface
        if ($launchResult.ExitCode -ne 0) {
            exit $launchResult.ExitCode
        }

        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        Start-Sleep -Milliseconds 1500
        while ([DateTime]::UtcNow -lt $deadline) {
            $gsPidLookup = Invoke-WslCommand -DistroName $Distro -CommandText ("pgrep -n -f '$MatchPattern'")
            $gsPid = $gsPidLookup.StdOut.Trim()
            if (-not [string]::IsNullOrWhiteSpace($gsPid)) {
                $writePid = Invoke-WslCommand -DistroName $Distro -CommandText ("echo '{0}' > '{1}'" -f $gsPid, $pidPath)
                if ($writePid.ExitCode -eq 0) {
                    $clientResult = Invoke-WslCommand -DistroName $Distro -CommandText ("python3 '{0}' tools-list >/dev/null 2>&1" -f $clientPath)
                    if ($clientResult.ExitCode -eq 0) {
                        $wslIp = Get-WslPrimaryIp -DistroName $Distro
                        if ([string]::IsNullOrWhiteSpace($wslIp)) {
                            exit 1
                        }

                        if (-not (Ensure-WindowsMcpProxy -ConnectAddress $wslIp)) {
                            exit 1
                        }

                        Write-Output ("READY:" + $gsPid + ":" + $wslIp)
                        exit 0
                    }
                }
            }

            Start-Sleep -Milliseconds 500
        }

        exit 1
    }

    'stop_mcp' {
        $matchPattern = Get-WslGsMatchPattern -Interface ".*"
        $stopResult = Stop-WslGsInstance -DistroName $Distro -MatchPattern $matchPattern
        Remove-WindowsMcpProxy
        exit 0
    }

    'mcp_ready' {
        if ([string]::IsNullOrWhiteSpace($Arg1)) {
            throw 'mcp_ready requires the WSL MCP client path.'
        }

        $clientPath = Escape-BashSingleQuotedText $Arg1
        $clientResult = Invoke-WslCommand -DistroName $Distro -CommandText ("python3 '{0}' tools-list >/dev/null 2>&1" -f $clientPath)
        exit $clientResult.ExitCode
    }

    default {
        throw "Unsupported action '$Action'."
    }
}
