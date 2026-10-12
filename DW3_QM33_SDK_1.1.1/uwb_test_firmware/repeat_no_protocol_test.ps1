[CmdletBinding()]
param(
    [ValidateRange(1, 1000)] [int] $Rounds = 1,
    [ValidateRange(1, 86400)] [int] $TimeoutSeconds = 900,
    [ValidatePattern('^\d+$')] [string] $TxSerial = '1050244938',
    [ValidatePattern('^\d+$')] [string] $RxSerial = '1050238642',
    [ValidatePattern('^0x[0-9a-fA-F]+$')] [string] $RxRttAddress = '0x20000048',
    [ValidatePattern('^0x[0-9a-fA-F]+$')] [string] $TxRttAddress = '0x20000050',
    [string] $JLinkDirectory = 'C:\Program Files\SEGGER\JLink_V980'
)

# Reuse the firmware already installed on the two boards. No build/erase/loadfile.
$ErrorActionPreference = 'Stop'
$Target = & (Join-Path $PSScriptRoot 'get_packet_target.ps1')
$jlink = Join-Path $JLinkDirectory 'JLink.exe'
$logger = Join-Path $JLinkDirectory 'JLinkRTTLogger.exe'
foreach ($file in @($jlink, $logger)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Required tool not found: $file" }
}
if ($TxSerial -eq $RxSerial) { throw 'TX and RX must use different probes.' }
$session = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$directory = Join-Path $PSScriptRoot "repeat_$session"
$null = New-Item -ItemType Directory -Path $directory
$results = [Collections.Generic.List[object]]::new()
# Prevent two copies of this script from controlling the same pair concurrently.
$mutex = [Threading.Mutex]::new($false, "Local\UwbRepeat_${TxSerial}_${RxSerial}")
$locked = $false
$tx = $null
$rx = $null
$monitor = $null

function Start-Commander([string] $Serial, [string] $Log, [string[]] $Commands) {
    $script = "$Log.jlink"
    $Commands | Set-Content -LiteralPath $script -Encoding ASCII
    # Use CommanderScript rather than redirected interactive stdin: this J-Link
    # version does not reliably consume commands arriving on redirected stdin.
    $process = Start-Process -FilePath $jlink -ArgumentList @(
        '-Device', 'Cortex-M4', '-If', 'SWD', '-Speed', '100', '-USB', $Serial,
        '-AutoConnect', '1', '-ExitOnError', '1', '-CommanderScript', ('"' + $script + '"')
    ) -RedirectStandardOutput $Log -RedirectStandardError "$Log.err" -WindowStyle Hidden -PassThru
    # Retain a process handle so Windows PowerShell can read ExitCode even if
    # the short RX reset process finishes before WaitForExit is called.
    $null = $process.Handle
    return [pscustomobject]@{ Process = $process; Log = $Log; Started = Get-Date }
}

function Wait-Commander($Commander, [string] $Pattern, [int] $Seconds = 45) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $Commander.Log) -and
            (Select-String -LiteralPath $Commander.Log -Pattern $Pattern -Quiet)) { return }
        if ($Commander.Process.HasExited) { break }
        Start-Sleep -Milliseconds 100
    }
    throw "J-Link command did not finish. See $($Commander.Log)"
}

function Close-Commander($Commander) {
    if ($null -eq $Commander) { return }
    if (-not $Commander.Process.HasExited) { $Commander.Process.Kill(); $Commander.Process.WaitForExit() }
    $Commander.Process.Dispose()
}

function Stop-Monitor($Process) {
    if ($null -ne $Process) {
        if (-not $Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
        $Process.Dispose()
    }
}

try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Another repeat test is already using these probes.' }
    Write-Host "Logs: $directory"
    for ($round = 1; $round -le $Rounds; $round++) {
        $prefix = Join-Path $directory "round_$round"
        $data = "$prefix.rx.log"
        Write-Host "Round $round/$Rounds : resetting TX and holding it before the first packet..."
        # Hold TX until RX setup is complete. On this board a direct 'g' while
        # Commander remains attached can stall startup; handing the connection
        # over to RTT Logger lets the installed firmware start reliably.
        $txHeld = $false
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            $tx = Start-Commander $TxSerial "$prefix.tx_control_$attempt.log" @(
                'h', 'r', 'h', 'regs', 'regs', 'regs', 'sleep 7200000', 'exit'
            )
            try {
                Wait-Commander $tx 'Reset: Reset device via AIRCR.SYSRESETREQ.' 15
                $txHeld = $true
                break
            } catch {
                Close-Commander $tx
                $tx = $null
                Write-Host "TX connect retry $attempt/3..."
            }
        }
        if (-not $txHeld) { throw "TX could not be reset and held. See $prefix.tx_control_*.log" }
        $resetOk = $false
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            $rx = Start-Commander $RxSerial "$prefix.rx_reset_$attempt.log" @('h', 'r', 'g', 'exit')
            if (-not $rx.Process.WaitForExit(8000)) { throw 'RX reset timed out.' }
            $resetOk = $rx.Process.ExitCode -eq 0
            Close-Commander $rx
            $rx = $null
            if ($resetOk) { break }
            Write-Host "RX connect retry $attempt/3..."
        }
        if (-not $resetOk) { throw "RX reset failed. See $prefix.rx_reset_*.log" }

        $monitor = Start-Process -FilePath $logger -ArgumentList @(
            '-Device', 'Cortex-M4', '-If', 'SWD', '-Speed', '100', '-USB', $RxSerial,
            '-RTTAddress', $RxRttAddress, '-RTTChannel', '0', ('"' + $data + '"')
        ) -RedirectStandardOutput "$prefix.rx_status.log" -RedirectStandardError "$prefix.rx_error.log" -WindowStyle Hidden -PassThru
        $ready = $false
        $deadline = (Get-Date).AddSeconds(45)
        while ((Get-Date) -lt $deadline) {
            if ($tx.Process.HasExited) { throw "TX controller exited. See $($tx.Log)" }
            if ($monitor.HasExited) { throw "RX logger exited. See $prefix.rx_status.log" }
            if (Test-Path -LiteralPath $data) {
                $content = Get-Content -LiteralPath $data -Raw
                if ($content -match 'TEST START target=(\d+)') {
                    if ([int]$Matches[1] -ne $Target) { throw "Shared config target=$Target differs from installed RX target=$($Matches[1]). Rebuild and flash both boards for the new configuration before repeating." }
                    if ($content -match '(?m)^RX_(SIGNAL seq=|PROGRESS |COMPLETE )') { throw 'RX received data before TX was released. Close other test programs and retry.' }
                    $ready = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $ready) { throw "RX was not ready. See $data and $prefix.rx_status.log" }

        Close-Commander $tx
        $tx = $null
        $runStart = Get-Date
        $txData = "$prefix.tx.log"
        $txProcess = Start-Process -FilePath $logger -ArgumentList @(
            '-Device', 'Cortex-M4', '-If', 'SWD', '-Speed', '100', '-USB', $TxSerial,
            '-RTTAddress', $TxRttAddress, '-RTTChannel', '0', ('"' + $txData + '"')
        ) -RedirectStandardOutput "$prefix.tx_status.log" -RedirectStandardError "$prefix.tx_error.log" -WindowStyle Hidden -PassThru
        $null = $txProcess.Handle
        $tx = [pscustomobject]@{ Process = $txProcess; Log = "$prefix.tx_status.log" }
        $lastProgress = ''
        $nextUpdate = $runStart
        $completed = $false
        Write-Host "RX ready; TX monitor connected. Target: $Target packets."
        while (((Get-Date) - $runStart).TotalSeconds -lt $TimeoutSeconds) {
            if ($tx.Process.HasExited -or $monitor.HasExited) { throw 'A probe connection closed during the test. See control/status logs.' }
            if (Test-Path -LiteralPath $data) {
                $tail = @(Get-Content -LiteralPath $data -Tail 12)
                if ($tail -match '^RX_COMPLETE ') { $completed = $true; break }
                if (((Get-Date) - $runStart).TotalSeconds -gt 90 -and
                    -not ($tail -match '^RX_(SIGNAL seq=|PROGRESS )')) { throw "No received packets within 90 seconds. See $txData and $data" }
                if ((Get-Date) -ge $nextUpdate) {
                    $progress = $tail | Where-Object { $_ -match '^RX_PROGRESS ' } | Select-Object -Last 1
                    if ($progress -and $progress -ne $lastProgress) { Write-Host $progress; $lastProgress = $progress }
                    else { Write-Host ("Waiting... {0:N0} seconds" -f ((Get-Date) - $runStart).TotalSeconds) }
                    $nextUpdate = (Get-Date).AddSeconds(15)
                }
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $completed) { throw "Timed out after $TimeoutSeconds seconds. See $data" }
        # Allow the transmitter to finish its repeated END markers before cleanup.
        Start-Sleep -Milliseconds 1000
        Stop-Monitor $monitor
        $monitor = $null
        $elapsedSeconds = ((Get-Date) - $runStart).TotalSeconds
        # The CRC-protected END marker received by RX is sufficient confirmation
        # of the sender's target. TX RTT may remain inaccessible on these boards
        # even while TX is sending; it is optional evidence, not a result gate.
        $row = & (Join-Path $PSScriptRoot 'read_no_protocol_result.ps1') -InputLog $data `
            -Round $round -ObservedSeconds $elapsedSeconds -TxLog $txData -OutputCsv "$prefix.result.csv"
        $results.Add($row)
        $results | Export-Csv -LiteralPath (Join-Path $directory 'results.csv') -NoTypeInformation -Encoding UTF8
        & (Join-Path $PSScriptRoot 'export_rx_signal.ps1') -InputLog $data
        Write-Host ("COMPLETE: {0}/{1}, success={2:N3}%, lost={3}, RSSI avg={4} dBm, FP avg={5} dBm" -f $row.received_unique, $Target, $row.success_percent, $row.lost_packets, $row.rssi_avg_dbm, $row.fp_avg_dbm) -ForegroundColor Green
        Write-Host ("RSSI (dBm): avg={0}, min={1}, max={2}" -f $row.rssi_avg_dbm, $row.rssi_min_dbm, $row.rssi_max_dbm)
        Write-Host ("First path (dBm): avg={0}, min={1}, max={2}; average gap={3} dB" -f $row.fp_avg_dbm, $row.fp_min_dbm, $row.fp_max_dbm, $row.gap_avg_db)
        if (-not $row.timing_valid) { Write-Warning 'Timing field is implausible; throughput has been left blank. Packet and signal statistics passed count checks.' }
        Close-Commander $tx
        $tx = $null
    }
    Write-Host "RESULT_CSV=$(Join-Path $directory 'results.csv')"
}
finally {
    Stop-Monitor $monitor
    Close-Commander $rx
    Close-Commander $tx
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
