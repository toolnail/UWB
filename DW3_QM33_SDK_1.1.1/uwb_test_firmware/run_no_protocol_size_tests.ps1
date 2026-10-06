param(
    [int[]] $Targets = @(100000, 200000, 300000, 400000),
    [int] $IntervalMs = 1
)

$ErrorActionPreference = 'Stop'

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
$selectionOriginal = [regex]::Replace($selectionOriginal, '(?m)^#define (TEST_PACKET_TARGET|TX_DELAY_MS)[^\r\n]*\r?\n', '')
$selectionOriginal = [regex]::Replace($selectionOriginal, '(?m)^#define TEST_SIMPLE_(RX|TX)[ \t]*(?=\r?$)', '//#define TEST_SIMPLE_$1')
$selectionOriginal = [regex]::new('(?m)^//#define TEST_SIMPLE_RX(?=\r?$)').Replace($selectionOriginal, '#define TEST_SIMPLE_RX', 1)
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$results = [System.Collections.Generic.List[object]]::new()

function Stop-RunLogger {
    param([System.Diagnostics.Process] $Process)
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit()
    }
}

function Set-ExampleSelection {
    param([ValidateSet('TX', 'RX')] [string] $Role, [int] $Target)

    $content = [regex]::Replace($selectionOriginal, '(?m)^#define TEST_SIMPLE_(RX|TX)[ \t]*(?=\r?$)', '//#define TEST_SIMPLE_$1')
    $content = [regex]::Replace($content, '(?m)^#define (TEST_PACKET_TARGET|TX_DELAY_MS)[^\r\n]*\r?\n', '')
    $macros = "#define TEST_SIMPLE_$Role`r`n#define TEST_PACKET_TARGET ${Target}UL`r`n#define TX_DELAY_MS ${IntervalMs}U`r`n"
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

foreach ($required in @($emBuild, $jlink, $logger, $selectionPath, $projectPath)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file not found: $required" }
}

$activeRxLogger = $null
$activeTxLogger = $null
$activeTxRunner = $null
try {
    foreach ($target in $Targets) {
        if ($target -le 0) { throw "Target must be positive: $target" }
        $runTag = "${target}_${session}"
        Write-Output "[$(Get-Date -Format s)] START no-protocol target=$target interval_ms=$IntervalMs"
        Stop-PreviousTx -Target $target

        Set-ExampleSelection -Role RX -Target $target
        $rxHex = Invoke-Build -Role RX -Target $target
        $rxRttAddress = Get-RttAddress
        Invoke-Flash -Serial $rxSerial -Image $rxHex -Label "rx_$target"
        $activeRxLogger = Start-RttLogger -Serial $rxSerial -Address $rxRttAddress -Label "rx_$target"
        if (-not (Wait-ForPattern -Path $activeRxLogger.Data `
                                -Pattern "TEST START target=$target" -TimeoutSeconds 45)) {
            throw "RX did not become ready for target=$target. See $($activeRxLogger.Data) and $($activeRxLogger.Status)"
        }

        Set-ExampleSelection -Role TX -Target $target
        $txHex = Invoke-Build -Role TX -Target $target
        $runStart = Get-Date
        Invoke-Flash -Serial $txSerial -Image $txHex -Label "tx_$target"
        # Keep a debugger connection on TX while it starts. On these boards the
        # flash command's final disconnect can leave TX waiting to run.
        $activeTxLogger = Start-RttLogger -Serial $txSerial -Address (Get-RttAddress) -Label "tx_$target"
        if (-not (Wait-ForPattern -Path $activeRxLogger.Data -Pattern 'RX_PROGRESS' -TimeoutSeconds 180)) {
            throw "No received packets after TX flash for target=$target. See $($activeRxLogger.Data)"
        }
        # Radio transmission and firmware polling add time beyond Sleep(IntervalMs).
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
        # 'lost' is the residual failure bucket; PHY errors without a readable
        # sequence cannot be separated from absent frames or added again.
        if ([int64]$rx['target'] -ne $target -or [int64]$rx['end_seq'] -ne $target -or
            ($success + $failed) -ne $target -or
            ([int64]$rx['lost'] + [int64]$rx['invalid_failed']) -ne $failed -or
            [int64]$rx['crc_err'] -gt [int64]$rx['phy_err']) {
            throw "Count mismatch or missing completion marker for target=$target. RX=$rxLine"
        }
        $txElapsedUs = [int64]$rx['tx_elapsed_us']
        $payloadBytes = [int64]$rx['payload_bytes']
        $goodputBps = [int64]$rx['goodput_bps']
        if ($txElapsedUs -le 0 -or $payloadBytes -ne 21) {
            throw "Missing on-device TX elapsed time for target=$target. RX=$rxLine"
        }
        $expectedGoodputBps = [math]::Round($success * $payloadBytes * 8.0 * 1000000.0 / $txElapsedUs)
        if ([math]::Abs($goodputBps - $expectedGoodputBps) -gt 1) {
            throw "Firmware goodput disagrees with packet counts for target=$target. RX=$rxLine"
        }

        $row = [pscustomobject]@{
            protocol = 'none'
            interval_ms = $IntervalMs
            target_packets = $target
            tx_packets_confirmed = [int64]$rx['end_seq']
            successful_unique_packets = $success
            failed_packets = $failed
            lost_packets = [int64]$rx['lost']
            invalid_failed_packets = [int64]$rx['invalid_failed']
            success_rate_percent = [math]::Round($success * 100.0 / $target, 6)
            packet_failure_rate_percent = [math]::Round($failed * 100.0 / $target, 6)
            success_ppm = [int64]$rx['success_ppm']
            duplicate_frames = [int64]$rx['dup']
            out_of_order_frames = [int64]$rx['ooo']
            invalid_frames = [int64]$rx['invalid']
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
    }
    Write-Output "RESULT_CSV=$resultCsv"
}
finally {
    Stop-RunLogger -Process $activeTxLogger.Process
    Stop-RunLogger -Process $activeTxRunner.Process
    Stop-RunLogger -Process $activeRxLogger.Process
    [System.IO.File]::WriteAllText($selectionPath, $selectionOriginal, $utf8NoBom)
}
