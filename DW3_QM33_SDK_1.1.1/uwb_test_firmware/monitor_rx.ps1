$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'UWB RX - J-Link 1050238642'

$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$session = Get-Date -Format 'yyyyMMdd_HHmmss'
$dataLog = Join-Path $PSScriptRoot "rx_live_$session.log"
$statusLog = Join-Path $PSScriptRoot "rx_logger_status_$session.log"
$errorLog = Join-Path $PSScriptRoot "rx_logger_error_$session.log"
$loggerArgs = @(
    '-Device', 'nRF52840_xxAA',
    '-If', 'SWD',
    '-Speed', '1000',
    '-USB', '1050238642',
    '-RTTAddress', '0x20000048',
    '-RTTChannel', '0',
    $dataLog
)

Write-Host 'UWB RX monitor (J-Link 1050238642)' -ForegroundColor Green
Write-Host "RTT log: $dataLog"
Write-Host 'Expected: SIMPLE RX v1.0 / Frame Received' -ForegroundColor Yellow
Write-Host 'Press Ctrl+C or close this window to stop.'
Write-Host ''

$loggerProcess = Start-Process -FilePath $logger -ArgumentList $loggerArgs `
    -RedirectStandardOutput $statusLog -RedirectStandardError $errorLog `
    -WindowStyle Hidden -PassThru

try {
    $deadline = (Get-Date).AddSeconds(15)
    while (-not (Test-Path -LiteralPath $dataLog) -and -not $loggerProcess.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }

    if (-not (Test-Path -LiteralPath $dataLog)) {
        Write-Host 'RTT connection failed:' -ForegroundColor Red
        if (Test-Path -LiteralPath $statusLog) { Get-Content -LiteralPath $statusLog }
        if (Test-Path -LiteralPath $errorLog) { Get-Content -LiteralPath $errorLog }
        Read-Host 'Press Enter to close'
        exit 1
    }

    Get-Content -LiteralPath $dataLog -Wait
}
finally {
    if ($null -ne $loggerProcess -and -not $loggerProcess.HasExited) {
        Stop-Process -Id $loggerProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
