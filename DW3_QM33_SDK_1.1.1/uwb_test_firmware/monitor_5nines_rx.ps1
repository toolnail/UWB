$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'UWB 5NINES RX - J-Link 1050238642'
$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$session = Get-Date -Format 'yyyyMMdd_HHmmss'
$dataLog = Join-Path $PSScriptRoot "5nines_rx_live_$session.log"
$statusLog = Join-Path $PSScriptRoot "5nines_rx_status_$session.log"
$errorLog = Join-Path $PSScriptRoot "5nines_rx_error_$session.log"
$argsList = @('-Device','nRF52840_xxAA','-If','SWD','-Speed','1000','-USB','1050238642',
              '-RTTAddress','0x20000048','-RTTChannel','0',$dataLog)

Write-Host 'UWB five-nines RX test' -ForegroundColor Green
Write-Host 'Strict pass: 300000 total, lost=0, invalid=0, RESULT=PASS_95CI'
Write-Host "Log: $dataLog"
$p = Start-Process -FilePath $logger -ArgumentList $argsList -RedirectStandardOutput $statusLog `
     -RedirectStandardError $errorLog -WindowStyle Hidden -PassThru
try {
    $deadline = (Get-Date).AddSeconds(30)
    while (-not (Test-Path -LiteralPath $dataLog) -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-Path -LiteralPath $dataLog)) {
        Write-Host 'RTT connection failed:' -ForegroundColor Red
        if (Test-Path $statusLog) { Get-Content $statusLog }
        if (Test-Path $errorLog) { Get-Content $errorLog }
        Read-Host 'Press Enter to close'
        exit 1
    }
    Get-Content -LiteralPath $dataLog -Wait
}
finally {
    if ($null -ne $p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
