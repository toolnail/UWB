param(
    [int] $TimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'

$jlink = 'C:\Program Files\SEGGER\JLink_V980\JLink.exe'
$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$txSerial = '1050244938'
$rxSerial = '1050238642'
$target = 600000
$session = Get-Date -Format 'yyyyMMdd_HHmmss'

$txHex = Join-Path $PSScriptRoot 'nrf52840_dwm3000_reliable_p2p_tx.hex'
$rxHex = Join-Path $PSScriptRoot 'nrf52840_dwm3000_reliable_p2p_rx.hex'
$txScript = Join-Path $PSScriptRoot 'flash_reliable_p2p_tx.jlink'
$rxScript = Join-Path $PSScriptRoot 'flash_reliable_p2p_rx.jlink'
$txLog = Join-Path $PSScriptRoot "reliable_p2p_${session}_tx.log"
$rxLog = Join-Path $PSScriptRoot "reliable_p2p_${session}_rx.log"
$txStatus = Join-Path $PSScriptRoot "reliable_p2p_${session}_tx_status.log"
$rxStatus = Join-Path $PSScriptRoot "reliable_p2p_${session}_rx_status.log"
$txError = Join-Path $PSScriptRoot "reliable_p2p_${session}_tx_error.log"
$rxError = Join-Path $PSScriptRoot "reliable_p2p_${session}_rx_error.log"
$txFlash = Join-Path $PSScriptRoot "reliable_p2p_${session}_tx_flash.log"
$rxFlash = Join-Path $PSScriptRoot "reliable_p2p_${session}_rx_flash.log"
$resultCsv = Join-Path $PSScriptRoot "reliable_p2p_${session}_summary.csv"

function Stop-TestLoggers {
    Get-Process JLinkRTTLogger -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

function Invoke-Flash {
    param(
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] [string] $Script,
        [Parameter(Mandatory)] [string] $Log
    )

    & $jlink -Device nRF52840_xxAA -If SWD -Speed 1000 -USB $Serial `
        -CommanderScript $Script *> $Log
    if ($LASTEXITCODE -ne 0 -or
        -not (Select-String -LiteralPath $Log -Pattern '^O\.K\.$' -Quiet)) {
        throw "J-Link flash failed for $Serial. See $Log"
    }
}

function Start-RttLogger {
    param(
        [Parameter(Mandatory)] [string] $Serial,
        [string] $Address,
        [Parameter(Mandatory)] [string] $DataLog,
        [Parameter(Mandatory)] [string] $StatusLog,
        [Parameter(Mandatory)] [string] $ErrorLog
    )

    $arguments = @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000',
                   '-USB', $Serial)
    if ($Address) {
        $arguments += @('-RTTAddress', $Address)
    }
    $arguments += @('-RTTChannel', '0', $DataLog)
    Start-Process -FilePath $logger -ArgumentList $arguments `
        -RedirectStandardOutput $StatusLog -RedirectStandardError $ErrorLog `
        -WindowStyle Hidden -PassThru
}

function Wait-ForPattern {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Pattern,
        [Parameter(Mandatory)] [int] $Timeout
    )

    $deadline = (Get-Date).AddSeconds($Timeout)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $Path) -and
            (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Parse-KeyValues {
    param([Parameter(Mandatory)] [string] $Line)

    $values = @{}
    foreach ($match in [regex]::Matches($Line, '(?<key>[A-Za-z_]+)=(?<value>\S+)')) {
        $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
    }
    return $values
}

foreach ($required in @($jlink, $logger, $txHex, $rxHex, $txScript, $rxScript)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required file not found: $required"
    }
}

$rxLogger = $null
$txLogger = $null

try {
    Stop-TestLoggers

    Write-Output "[$(Get-Date -Format s)] Flashing reliable receiver"
    Invoke-Flash -Serial $rxSerial -Script $rxScript -Log $rxFlash
    $rxLogger = Start-RttLogger -Serial $rxSerial -Address '0x20000058' `
        -DataLog $rxLog -StatusLog $rxStatus -ErrorLog $rxError

    if (-not (Wait-ForPattern -Path $rxLog -Pattern "RX READY target=$target" -Timeout 30)) {
        throw "Receiver did not become ready. See $rxLog and $rxStatus"
    }

    Write-Output "[$(Get-Date -Format s)] Flashing reliable transmitter"
    Invoke-Flash -Serial $txSerial -Script $txScript -Log $txFlash
    $runStart = Get-Date
    $txLogger = Start-RttLogger -Serial $txSerial -Address '0x20000050' `
        -DataLog $txLog -StatusLog $txStatus -ErrorLog $txError

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $nextProgress = (Get-Date).AddSeconds(30)
    $complete = $false

    while ((Get-Date) -lt $deadline) {
        $rxDone = (Test-Path -LiteralPath $rxLog) -and
                  (Select-String -LiteralPath $rxLog -Pattern 'RX_COMPLETE' -Quiet)
        $txDone = (Test-Path -LiteralPath $txLog) -and
                  (Select-String -LiteralPath $txLog -Pattern 'TX_COMPLETE' -Quiet)
        $txAbort = (Test-Path -LiteralPath $txLog) -and
                   (Select-String -LiteralPath $txLog -Pattern 'TX_ABORT' -Quiet)

        if ($txAbort) {
            throw "Transmitter aborted. See $txLog"
        }
        if ($rxDone -and $txDone) {
            $complete = $true
            break
        }

        if ((Get-Date) -ge $nextProgress) {
            $progress = Select-String -LiteralPath $txLog -Pattern 'TX_PROGRESS' `
                -ErrorAction SilentlyContinue | Select-Object -Last 1 -ExpandProperty Line
            if ($progress) {
                Write-Output "[$(Get-Date -Format s)] $progress"
            }
            else {
                Write-Output "[$(Get-Date -Format s)] waiting for TX progress"
            }
            $nextProgress = (Get-Date).AddSeconds(30)
        }
        Start-Sleep -Seconds 1
    }

    if (-not $complete) {
        throw "Timed out waiting for TX/RX completion. See $txLog and $rxLog"
    }

    $elapsed = [math]::Round(((Get-Date) - $runStart).TotalSeconds, 3)
    $txLine = Select-String -LiteralPath $txLog -Pattern 'TX_COMPLETE' |
        Select-Object -Last 1 -ExpandProperty Line
    $rxLine = Select-String -LiteralPath $rxLog -Pattern 'RX_COMPLETE' |
        Select-Object -Last 1 -ExpandProperty Line
    $tx = Parse-KeyValues -Line $txLine
    $rx = Parse-KeyValues -Line $rxLine

    $unique = [int64]$rx['unique']
    $failed = [int64]$target - $unique
    $invalid = [int64]$rx['invalid']
    $attempts = [int64]$tx['attempts']
    $retransmissions = [int64]$tx['retransmissions']

    $row = [pscustomobject]@{
        protocol = 'IEEE802.15.4 + 32-packet Selective Repeat ARQ'
        interval_ms = 1
        window_size = 32
        target_packets = $target
        received_unique = $unique
        failed_packets = $failed
        invalid = $invalid
        duplicates = [int64]$rx['duplicates']
        phy_errors = [int64]$rx['phy_err']
        tx_attempts = $attempts
        retransmissions = $retransmissions
        retransmission_rate_percent = [math]::Round($retransmissions * 100.0 / $attempts, 6)
        ack_requests = [int64]$tx['ack_requests']
        ack_timeouts = [int64]$tx['ack_timeouts']
        retried_windows = [int64]$tx['retried_windows']
        packet_failure_rate_percent = [math]::Round($failed * 100.0 / $target, 6)
        success_rate_percent = [math]::Round($unique * 100.0 / $target, 6)
        success_ppm = [math]::Floor($unique * 1000000.0 / $target)
        tx_result = $tx['RESULT']
        rx_result = $rx['RESULT']
        elapsed_seconds = $elapsed
        tx_log = $txLog
        rx_log = $rxLog
    }
    $row | Export-Csv -LiteralPath $resultCsv -NoTypeInformation -Encoding UTF8

    Write-Output "TX_RESULT: $txLine"
    Write-Output "RX_RESULT: $rxLine"
    Write-Output "RESULT_CSV=$resultCsv"
}
finally {
    if ($rxLogger -and -not $rxLogger.HasExited) {
        Stop-Process -Id $rxLogger.Id -Force -ErrorAction SilentlyContinue
    }
    if ($txLogger -and -not $txLogger.HasExited) {
        Stop-Process -Id $txLogger.Id -Force -ErrorAction SilentlyContinue
    }
}
