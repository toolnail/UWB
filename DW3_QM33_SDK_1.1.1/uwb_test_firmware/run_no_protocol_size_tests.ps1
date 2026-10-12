[CmdletBinding()]
param(
    [int] $IntervalUs = 500,
    [ValidateRange(0, 4294967295)] [long] $SignalLogEvery = 1000,
    [switch] $DisableSignal,
    [switch] $BuildOnly
)

$ErrorActionPreference = 'Stop'
$target = & (Join-Path $PSScriptRoot 'get_packet_target.ps1')

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$emBuild = 'C:\Program Files\SEGGER\SEGGER Embedded Studio for ARM 5.10a\bin\emBuild.exe'
$jlink = 'C:\Program Files\SEGGER\JLink_V980\JLink.exe'
$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$txSerial = '1050244938'
$rxSerial = '1050238642'
$selectionPath = Join-Path $workspace 'Drivers\API\Src\example_selection.h'
$projectPath = Join-Path $workspace 'Drivers\API\Build_Platforms\nRF52840-DK\dw3000_api.emProject'
$buildHex = Join-Path $workspace 'Drivers\API\Build_Platforms\nRF52840-DK\Output\Debug\Exe\dw3000_api.hex'
$buildMap = Join-Path $workspace 'Drivers\API\Build_Platforms\nRF52840-DK\Output\Debug\Exe\dw3000_api.map'
$session = Get-Date -Format 'yyyyMMdd_HHmmss'
$resultCsv = Join-Path $PSScriptRoot "no_protocol_sizes_${session}.csv"
$selectionOriginal = [System.IO.File]::ReadAllText($selectionPath)
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$results = [System.Collections.Generic.List[object]]::new()
$signalEnabled = if ($DisableSignal) { 0 } else { 1 }

function Stop-RunLogger {
    param([System.Diagnostics.Process] $Process)
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit()
    }
}

function Set-ExampleSelection {
    param([ValidateSet('TX', 'RX')] [string] $Role)

    $content = [regex]::Replace($selectionOriginal, '(?m)^#define TEST_SIMPLE_(RX|TX)[ \t]*(?=\r?$)', '//#define TEST_SIMPLE_$1')
    $content = [regex]::Replace($content, '(?m)^#define (TX_DELAY_MS|TX_DELAY_US|TEST_SIGNAL_ENABLE|TEST_SIGNAL_LOG_EVERY)[^\r\n]*\r?\n', '')
    $macros = "#define TEST_SIMPLE_$Role`r`n#define TX_DELAY_US ${IntervalUs}U`r`n#define TEST_SIGNAL_ENABLE ${signalEnabled}`r`n#define TEST_SIGNAL_LOG_EVERY ${SignalLogEvery}UL`r`n"
    $content = [regex]::new('#ifdef __cplusplus').Replace($content, "$macros`r`n#ifdef __cplusplus", 1)
    $activeRoles = [regex]::Matches($content, '(?m)^#define TEST_SIMPLE_(RX|TX)[ \t]*\r?$')
    if ($activeRoles.Count -ne 1 -or $activeRoles[0].Groups[1].Value -ne $Role) {
        throw "Invalid example selection for $Role target=$Target"
    }
    [System.IO.File]::WriteAllText($selectionPath, $content, $utf8NoBom)
}

function Get-RttAddress {
    $match = Select-String -LiteralPath $buildMap -Pattern '^\s*(0x[0-9a-fA-F]+)\s+_SEGGER_RTT\s*$' |
        Select-Object -Last 1
    if (-not $match) { throw "RTT address not found in $buildMap" }
    return $match.Matches[0].Groups[1].Value
}

function Invoke-Build {
    param([string] $Role, [int] $Target)

    if ((& (Join-Path $PSScriptRoot 'get_packet_target.ps1')) -ne $Target) {
        throw 'Shared packet configuration changed during the build. Run again with a stable configuration.'
    }

    $log = Join-Path $PSScriptRoot "build_no_protocol_${Role}_${Target}_${session}.log"
    $stderr = Join-Path $PSScriptRoot "build_no_protocol_${Role}_${Target}_${session}.err.log"
    $process = Start-Process -FilePath $emBuild `
        -ArgumentList @('-config', 'Debug', '-rebuild', $projectPath) `
        -WorkingDirectory (Split-Path $projectPath) `
        -RedirectStandardOutput $log -RedirectStandardError $stderr `
        -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $buildHex)) {
        throw "Build failed for $Role target=$Target. See $log and $stderr"
    }

    $image = Join-Path $PSScriptRoot "nrf52840_dwm3000_5nines_${Role}_${Target}_${session}.hex"
    Copy-Item -LiteralPath $buildHex -Destination $image -Force
    Copy-Item -LiteralPath $buildMap -Destination ([IO.Path]::ChangeExtension($image, 'map')) -Force
    return $image
}

function Invoke-Flash {
    param([string] $Serial, [string] $Image, [string] $Label)

    $script = Join-Path $PSScriptRoot "flash_no_protocol_${Label}_${session}.jlink"
    $log = Join-Path $PSScriptRoot "flash_no_protocol_${Label}_${session}.log"
    @('r', 'h', 'erase', "loadfile $Image", 'r', 'g', 'sleep 2000', 'exit') |
        Set-Content -LiteralPath $script -Encoding ASCII
    $process = Start-Process -FilePath $jlink `
        -ArgumentList @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000',
                        '-USB', $Serial, '-CommanderScript', $script) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0 -or
        -not (Select-String -LiteralPath $log -Pattern '^O\.K\.$' -Quiet)) {
        throw "Flash failed for $Label. See $log"
    }
}

function Stop-PreviousTx {
    param([int] $Target)

    $script = Join-Path $PSScriptRoot "halt_old_tx_${Target}_${session}.jlink"
    $log = Join-Path $PSScriptRoot "halt_old_tx_${Target}_${session}.log"
    @('h', 'exit') | Set-Content -LiteralPath $script -Encoding ASCII
    $process = Start-Process -FilePath $jlink `
        -ArgumentList @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000',
                        '-USB', $txSerial, '-CommanderScript', $script) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0 -or
        -not (Select-String -LiteralPath $log -Pattern '^J-Link>h$' -Quiet)) {
        throw "Could not stop the previous transmitter. See $log"
    }
}

function Start-TxRunner {
    param([int] $Target)

    $script = Join-Path $PSScriptRoot "run_tx_${Target}_${session}.jlink"
    $log = Join-Path $PSScriptRoot "run_tx_${Target}_${session}.log"
    @('g', 'sleep 7200000', 'exit') | Set-Content -LiteralPath $script -Encoding ASCII
    $process = Start-Process -FilePath $jlink `
        -ArgumentList @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000',
                        '-USB', $txSerial, '-CommanderScript', $script) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WindowStyle Hidden -PassThru
    return [pscustomobject]@{ Process = $process; Log = $log }
}

function Start-RttLogger {
    param([string] $Serial, [string] $Address, [string] $Label)

    $data = Join-Path $PSScriptRoot "no_protocol_${Label}_${session}.log"
    $status = Join-Path $PSScriptRoot "no_protocol_${Label}_${session}.status.log"
    $error = Join-Path $PSScriptRoot "no_protocol_${Label}_${session}.error.log"
    $process = Start-Process -FilePath $logger `
        -ArgumentList @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000',
                        '-USB', $Serial, '-RTTAddress', $Address, '-RTTChannel', '0', $data) `
        -RedirectStandardOutput $status -RedirectStandardError $error `
        -WindowStyle Hidden -PassThru
    return [pscustomobject]@{ Process = $process; Data = $data; Status = $status; Error = $error }
}

function Wait-ForPattern {
    param([string] $Path, [string] $Pattern, [int] $TimeoutSeconds)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $Path) -and
            (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet -ErrorAction SilentlyContinue)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Parse-KeyValues {
    param([string] $Line)
    $values = @{}
    foreach ($match in [regex]::Matches($Line, '(?<key>[A-Za-z_]+)=(?<value>\S+)')) {
        $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
    }
    return $values
}

$requiredFiles = @($emBuild, $selectionPath, $projectPath)
if (-not $BuildOnly) { $requiredFiles += @($jlink, $logger) }
foreach ($required in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file not found: $required" }
}

$activeRxLogger = $null
$activeTxLogger = $null
$activeTxRunner = $null
try {
    Write-Output "[$(Get-Date -Format s)] START no-protocol target=$target interval_us=$IntervalUs"

    Set-ExampleSelection -Role RX
    $rxHex = Invoke-Build -Role RX -Target $target
    $rxRttAddress = Get-RttAddress
    Set-ExampleSelection -Role TX
    $txHex = Invoke-Build -Role TX -Target $target
    $txRttAddress = Get-RttAddress
    if ((& (Join-Path $PSScriptRoot 'get_packet_target.ps1')) -ne $target) {
        throw 'Shared packet configuration changed during the build. Run again with a stable configuration.'
    }
    if ($BuildOnly) {
        Write-Output "BUILD_ONLY target=$target RX_HEX=$rxHex RX_RTT=$rxRttAddress TX_HEX=$txHex TX_RTT=$txRttAddress"
        return
    }

    Stop-PreviousTx -Target $target
    Invoke-Flash -Serial $rxSerial -Image $rxHex -Label "rx_$target"
    $activeRxLogger = Start-RttLogger -Serial $rxSerial -Address $rxRttAddress -Label "rx_$target"
    if (-not (Wait-ForPattern -Path $activeRxLogger.Data `
                            -Pattern "TEST START target=$target" -TimeoutSeconds 45)) {
        throw "RX did not become ready for target=$target. See $($activeRxLogger.Data) and $($activeRxLogger.Status)"
    }

    $runStart = Get-Date
    Invoke-Flash -Serial $txSerial -Image $txHex -Label "tx_$target"
    # Keep a debugger connection on TX while it starts. On these boards the
    # flash command's final disconnect can leave TX waiting to run.
    $activeTxLogger = Start-RttLogger -Serial $txSerial -Address $txRttAddress -Label "tx_$target"
    if (-not (Wait-ForPattern -Path $activeRxLogger.Data -Pattern '^RX_(PROGRESS|COMPLETE) ' -TimeoutSeconds 180)) {
        throw "No received packets after TX flash for target=$target. See $($activeRxLogger.Data)"
    }
    # Radio transmission and firmware polling add time beyond deca_usleep(IntervalUs).
    $timeout = [int][math]::Ceiling($target * 0.02) + 300
    if (-not (Wait-ForPattern -Path $activeRxLogger.Data -Pattern 'RX_COMPLETE' -TimeoutSeconds $timeout)) {
        throw "Timed out waiting for RX_COMPLETE target=$target. See $($activeRxLogger.Data)"
    }

    $elapsed = [math]::Round(((Get-Date) - $runStart).TotalSeconds, 3)
    $rxLine = Select-String -LiteralPath $activeRxLogger.Data -Pattern 'RX_COMPLETE' |
        Select-Object -Last 1 -ExpandProperty Line
    $rx = Parse-KeyValues -Line $rxLine
    $success = [int64]$rx['success']
    $failed = [int64]$rx['failed']
    # All target sequences not successfully received are failed/lost.
    # PHY/CRC error events are diagnostics and must not be added to that count.
    if ([int64]$rx['target'] -ne $target -or [int64]$rx['end_seq'] -ne $target -or
        ($success + $failed) -ne $target -or
        [int64]$rx['lost'] -ne $failed -or
        [int64]$rx['crc_err'] -gt [int64]$rx['phy_err']) {
        throw "Count mismatch or missing completion marker for target=$target. RX=$rxLine"
    }
    $txElapsedUs = [int64]$rx['tx_elapsed_us']
    $payloadBytes = [int64]$rx['payload_bytes']
    $goodputBps = [int64]$rx['goodput_bps']
    if ($txElapsedUs -le 0 -or $payloadBytes -ne 26) {
        throw "Missing on-device TX elapsed time for target=$target. RX=$rxLine"
    }
    $expectedGoodputBps = [math]::Round($success * $payloadBytes * 8.0 * 1000000.0 / $txElapsedUs)
    if ([math]::Abs($goodputBps - $expectedGoodputBps) -gt 1) {
        throw "Firmware goodput disagrees with packet counts for target=$target. RX=$rxLine"
    }

    $signal = @{}
    $signalCsv = $null
    if (-not $DisableSignal) {
        $signalLine = Select-String -LiteralPath $activeRxLogger.Data -Pattern '\bRX_SIGNAL_COMPLETE\s' |
            Select-Object -Last 1 -ExpandProperty Line
        if (-not $signalLine) { throw "Missing RX_SIGNAL_COMPLETE in $($activeRxLogger.Data)" }
        $signal = Parse-KeyValues -Line $signalLine
        if (([int64]$signal['samples'] + [int64]$signal['invalid']) -ne $success) {
            throw "Signal sample count disagrees with unique packets: $signalLine"
        }
        $signalCsv = [IO.Path]::ChangeExtension($activeRxLogger.Data, 'signal.csv')
        & (Join-Path $PSScriptRoot 'export_rx_signal.ps1') -InputLog $activeRxLogger.Data -OutputCsv $signalCsv
    }

    $row = [pscustomobject]@{
        protocol = 'none'
        signal_enabled = $signalEnabled
        signal_log_every = $SignalLogEvery
        signal_samples = $signal['samples']
        signal_invalid = $signal['invalid']
        rssi_avg_dbm = $signal['rssi_avg_dbm']
        rssi_min_dbm = $signal['rssi_min_dbm']
        rssi_max_dbm = $signal['rssi_max_dbm']
        fp_avg_dbm = $signal['fp_avg_dbm']
        fp_min_dbm = $signal['fp_min_dbm']
        fp_max_dbm = $signal['fp_max_dbm']
        gap_avg_db = $signal['gap_avg_db']
        signal_csv = $signalCsv
        interval_us = $IntervalUs
        target_packets = $target
        tx_packets_confirmed = [int64]$rx['end_seq']
        successful_unique_packets = $success
        failed_packets = $failed
        lost_packets = [int64]$rx['lost']
        success_rate_percent = [math]::Round($success * 100.0 / $target, 6)
        packet_failure_rate_percent = [math]::Round($failed * 100.0 / $target, 6)
        success_ppm = [int64]$rx['success_ppm']
        duplicate_frames = [int64]$rx['dup']
        out_of_order_frames = [int64]$rx['ooo']
        phy_error_events = [int64]$rx['phy_err']
        crc_error_events = [int64]$rx['crc_err']
        result = $rx['RESULT']
        elapsed_seconds = $elapsed
        tx_elapsed_us = $txElapsedUs
        payload_bytes = $payloadBytes
        payload_goodput_bps = $goodputBps
        payload_goodput_kbps = [math]::Round($goodputBps / 1000.0, 3)
        tx_log = $activeTxLogger.Data
        rx_log = $activeRxLogger.Data
    }
    $results.Add($row)
    $results | Export-Csv -LiteralPath $resultCsv -NoTypeInformation -Encoding UTF8
    Write-Output "[$(Get-Date -Format s)] COMPLETE $rxLine elapsed_seconds=$elapsed"

    Stop-RunLogger -Process $activeTxLogger.Process
    Stop-RunLogger -Process $activeTxRunner.Process
    Stop-RunLogger -Process $activeRxLogger.Process
    $activeTxLogger = $null
    $activeTxRunner = $null
    $activeRxLogger = $null
    Write-Output "RESULT_CSV=$resultCsv"
}
finally {
    Stop-RunLogger -Process $activeTxLogger.Process
    Stop-RunLogger -Process $activeTxRunner.Process
    Stop-RunLogger -Process $activeRxLogger.Process
    [System.IO.File]::WriteAllText($selectionPath, $selectionOriginal, $utf8NoBom)
}
