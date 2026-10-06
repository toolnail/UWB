param(
    [int[]] $Intervals = @(5, 4, 3, 2, 1),
    [switch] $SkipTxLogger
)

$ErrorActionPreference = 'Stop'

$jlink = 'C:\Program Files\SEGGER\JLink_V980\JLink.exe'
$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$txSerial = '1050244938'
$rxSerial = '1050238642'
$target = 300000
$session = Get-Date -Format 'yyyyMMdd_HHmmss'
$resultCsv = Join-Path $PSScriptRoot "interval_matrix_$session.csv"
$standardTxHex = Join-Path $PSScriptRoot 'nrf52840_dwm3000_5nines_tx.hex'
$backup10msHex = Join-Path $PSScriptRoot 'nrf52840_dwm3000_5nines_tx_10ms.hex'
$rxHex = Join-Path $PSScriptRoot 'nrf52840_dwm3000_5nines_rx.hex'
$txFlashScript = Join-Path $PSScriptRoot 'flash_5nines_tx.jlink'
$rxFlashScript = Join-Path $PSScriptRoot 'flash_5nines_rx.jlink'
$results = [System.Collections.Generic.List[object]]::new()

function Stop-TestLoggers {
    Get-Process JLinkRTTLogger -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

function Invoke-Flash {
    param(
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] [string] $Script,
        [Parameter(Mandatory)] [string] $Log
    )

    & $jlink -Device nRF52840_xxAA -If SWD -Speed 1000 -USB $Serial -CommanderScript $Script *> $Log
    if ($LASTEXITCODE -ne 0) {
        throw "J-Link flash failed for $Serial. See $Log"
    }
    if (-not (Select-String -LiteralPath $Log -Pattern '^O\.K\.$' -Quiet)) {
        throw "J-Link did not report a verified load for $Serial. See $Log"
    }
}

function Start-RttLogger {
    param(
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] [string] $Address,
        [Parameter(Mandatory)] [string] $DataLog,
        [Parameter(Mandatory)] [string] $StatusLog,
        [Parameter(Mandatory)] [string] $ErrorLog
    )

    $argsList = @('-Device', 'nRF52840_xxAA', '-If', 'SWD', '-Speed', '1000', '-USB', $Serial,
                  '-RTTAddress', $Address, '-RTTChannel', '0', $DataLog)
    return Start-Process -FilePath $logger -ArgumentList $argsList -RedirectStandardOutput $StatusLog `
                         -RedirectStandardError $ErrorLog -WindowStyle Hidden -PassThru
}

function Wait-ForPattern {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Pattern,
        [Parameter(Mandatory)] [int] $TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path -LiteralPath $Path) -and (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
            return $true
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

if (-not (Test-Path -LiteralPath $backup10msHex)) {
    Copy-Item -LiteralPath $standardTxHex -Destination $backup10msHex
}

try {
    Stop-TestLoggers

    foreach ($interval in $Intervals) {
        $runStart = Get-Date
        $tag = "${interval}ms_$session"
        $variantHex = Join-Path $PSScriptRoot "nrf52840_dwm3000_5nines_tx_${interval}ms.hex"
        $rxData = Join-Path $PSScriptRoot "interval_${tag}_rx_live.log"
        $rxStatus = Join-Path $PSScriptRoot "interval_${tag}_rx_status.log"
        $rxError = Join-Path $PSScriptRoot "interval_${tag}_rx_error.log"
        $txData = Join-Path $PSScriptRoot "interval_${tag}_tx_live.log"
        $txStatus = Join-Path $PSScriptRoot "interval_${tag}_tx_status.log"
        $txError = Join-Path $PSScriptRoot "interval_${tag}_tx_error.log"
        $rxFlashLog = Join-Path $PSScriptRoot "interval_${tag}_rx_flash.log"
        $txFlashLog = Join-Path $PSScriptRoot "interval_${tag}_tx_flash.log"

        Write-Output "[$(Get-Date -Format s)] START interval=${interval}ms target=$target"
        Copy-Item -LiteralPath $variantHex -Destination $standardTxHex -Force

        Invoke-Flash -Serial $rxSerial -Script $rxFlashScript -Log $rxFlashLog
        $rxLogger = Start-RttLogger -Serial $rxSerial -Address '0x20000048' -DataLog $rxData `
                                    -StatusLog $rxStatus -ErrorLog $rxError
        if (-not (Wait-ForPattern -Path $rxData -Pattern 'TEST START target=300000' -TimeoutSeconds 30)) {
            throw "RX did not become ready for ${interval}ms. See $rxData and $rxStatus"
        }

        Invoke-Flash -Serial $txSerial -Script $txFlashScript -Log $txFlashLog
        $txLogger = $null
        if (-not $SkipTxLogger) {
            $txLogger = Start-RttLogger -Serial $txSerial -Address '0x20000050' -DataLog $txData `
                                        -StatusLog $txStatus -ErrorLog $txError
        }

        $timeoutSeconds = [int](($target * $interval / 1000.0) + 900)
        $deadline = (Get-Date).AddSeconds($timeoutSeconds)
        $nextProgress = (Get-Date).AddSeconds(30)
        $complete = $false
        while ((Get-Date) -lt $deadline) {
            if ((Test-Path -LiteralPath $rxData) -and
                (Select-String -LiteralPath $rxData -Pattern 'RX_COMPLETE' -Quiet)) {
                $complete = $true
                break
            }

            if ((Get-Date) -ge $nextProgress) {
                $lastProgress = Select-String -LiteralPath $rxData -Pattern 'RX_PROGRESS' -ErrorAction SilentlyContinue |
                                Select-Object -Last 1 -ExpandProperty Line
                if ($lastProgress) {
                    Write-Output "[$(Get-Date -Format s)] interval=${interval}ms $lastProgress"
                }
                else {
                    Write-Output "[$(Get-Date -Format s)] interval=${interval}ms waiting for first RX progress"
                }
                $nextProgress = (Get-Date).AddSeconds(30)
            }
            Start-Sleep -Seconds 2
        }

        if (-not $complete) {
            throw "Timed out waiting for RX_COMPLETE at ${interval}ms. See $rxData"
        }

        if (-not $SkipTxLogger) {
            [void](Wait-ForPattern -Path $txData -Pattern 'TX_COMPLETE' -TimeoutSeconds 30)
        }
        $completeLine = Select-String -LiteralPath $rxData -Pattern 'RX_COMPLETE' |
                        Select-Object -Last 1 -ExpandProperty Line
        $match = [regex]::Match($completeLine,
            'RESULT=(?<result>\S+) target=(?<target>\d+) success=(?<success>\d+) failed=(?<failed>\d+) success_ppm=(?<ppm>\d+) dup=(?<dup>\d+) ooo=(?<ooo>\d+) invalid=(?<invalid>\d+) phy_err=(?<phy>\d+)')
        if (-not $match.Success) {
            throw "Could not parse RX result: $completeLine"
        }

        $success = [int64]$match.Groups['success'].Value
        $failed = [int64]$match.Groups['failed'].Value
        $invalid = [int64]$match.Groups['invalid'].Value
        $elapsed = [math]::Round(((Get-Date) - $runStart).TotalSeconds, 3)
        $row = [pscustomobject]@{
            interval_ms = $interval
            target = $target
            total = [int64]$match.Groups['target'].Value
            success = $success
            failed = $failed
            duplicate = [int64]$match.Groups['dup'].Value
            out_of_order = [int64]$match.Groups['ooo'].Value
            invalid = $invalid
            phy_errors = [int64]$match.Groups['phy'].Value
            success_ppm = [int64]$match.Groups['ppm'].Value
            success_rate_percent = [math]::Round(($success * 100.0 / $target), 6)
            packet_failure_rate_percent = [math]::Round(($failed * 100.0 / $target), 6)
            result = $match.Groups['result'].Value
            elapsed_seconds = $elapsed
            rx_log = $rxData
            tx_log = $txData
        }
        $results.Add($row)
        $results | Export-Csv -LiteralPath $resultCsv -NoTypeInformation -Encoding UTF8
        Write-Output "[$(Get-Date -Format s)] COMPLETE interval=${interval}ms $completeLine elapsed_seconds=$elapsed"

        if ($rxLogger -and -not $rxLogger.HasExited) { Stop-Process -Id $rxLogger.Id -Force -ErrorAction SilentlyContinue }
        if ($txLogger -and -not $txLogger.HasExited) { Stop-Process -Id $txLogger.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 1
    }

    Write-Output "RESULT_CSV=$resultCsv"
}
finally {
    Stop-TestLoggers
    Copy-Item -LiteralPath $backup10msHex -Destination $standardTxHex -Force
}
