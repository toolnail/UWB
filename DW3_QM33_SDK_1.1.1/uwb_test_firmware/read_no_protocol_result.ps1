param(
    [Parameter(Mandatory)] [string] $InputLog,
    [ValidateRange(1, 2147483647)] [int] $Target = 300000,
    [int] $Round = 1,
    [double] $ObservedSeconds = 0,
    [string] $TxLog,
    [string] $OutputCsv
)

$ErrorActionPreference = 'Stop'
$lines = @(Get-Content -LiteralPath $InputLog)
if (@($lines | Where-Object { $_ -match '^P2P 5NINES RX' }).Count -ne 1 -or
    @($lines | Where-Object { $_ -match '^TEST START ' }).Count -ne 1 -or
    @($lines | Where-Object { $_ -match '^RX_COMPLETE ' }).Count -ne 1 -or
    @($lines | Where-Object { $_ -match '^RX_SIGNAL_COMPLETE ' }).Count -ne 1) {
    throw 'Expected one complete RX run; restarted, mixed or incomplete logs are rejected.'
}
function Read-Fields([string] $Line) {
    $fields = @{}
    foreach ($m in [regex]::Matches($Line, '(?<key>[A-Za-z_][A-Za-z_0-9]*)=(?<value>\S+)')) {
        $fields[$m.Groups['key'].Value] = $m.Groups['value'].Value
    }
    return $fields
}
$c = Read-Fields ($lines | Where-Object { $_ -match '^RX_COMPLETE ' })
$s = Read-Fields ($lines | Where-Object { $_ -match '^RX_SIGNAL_COMPLETE ' })
foreach ($key in @('target', 'end_seq', 'success', 'failed', 'lost', 'invalid_failed', 'success_ppm',
                  'dup', 'ooo', 'invalid', 'phy_err', 'crc_err', 'tx_elapsed_us', 'payload_bytes', 'goodput_bps')) {
    if (-not $c.ContainsKey($key) -or $c[$key] -notmatch '^\d+$') { throw "Missing/invalid packet field: $key" }
}
if ([long]$c.target -ne $Target -or [long]$c.end_seq -ne $Target -or
    [long]$c.success + [long]$c.failed -ne $Target -or
    [long]$c.lost + [long]$c.invalid_failed -ne [long]$c.failed -or
    [long]$c.success_ppm -ne [math]::Floor([long]$c.success * 1000000.0 / $Target)) {
    throw 'Final packet counts are inconsistent or target differs from installed firmware.'
}
foreach ($key in @('samples', 'invalid')) {
    if (-not $s.ContainsKey($key) -or $s[$key] -notmatch '^\d+$') { throw "Missing/invalid signal field: $key" }
}
if ([long]$s.samples + [long]$s.invalid -ne [long]$c.success) { throw 'Signal samples do not match successful unique packets.' }
$previous = 0L
foreach ($line in $lines | Where-Object { $_ -match '^RX_PROGRESS ' }) {
    $p = Read-Fields $line
    if (-not $p.ContainsKey('valid') -or [long]$p.valid -le $previous -or [long]$p.valid -gt [long]$c.success) {
        throw 'Progress count decreased or exceeds final count; the log may contain multiple runs.'
    }
    $previous = [long]$p.valid
}
$timingValid = $ObservedSeconds -gt 0 -and [long]$c.tx_elapsed_us -gt 0 -and
    [double]$c.tx_elapsed_us / 1000000 -ge $ObservedSeconds * 0.5 -and
    [double]$c.tx_elapsed_us / 1000000 -le $ObservedSeconds + 5
$txConfirmed = $TxLog -and (Test-Path -LiteralPath $TxLog) -and
    (Select-String -LiteralPath $TxLog -Pattern "^TX_COMPLETE sent=$Target " -Quiet)
$result = [ordered]@{
    round = $Round; target_packets = $Target; received_unique = [long]$c.success
    failed_packets = [long]$c.failed; success_percent = 100.0 * [long]$c.success / $Target
    loss_percent = 100.0 * [long]$c.failed / $Target
    lost_packets = [long]$c.lost; invalid_failed = [long]$c.invalid_failed
    duplicate_packets = [long]$c.dup; out_of_order_packets = [long]$c.ooo
    phy_errors = [long]$c.phy_err; crc_errors = [long]$c.crc_err
    signal_samples = [long]$s.samples; signal_invalid = [long]$s.invalid
    timing_valid = [bool]$timingValid; raw_tx_elapsed_us = [long]$c.tx_elapsed_us
    raw_goodput_bps = [long]$c.goodput_bps
    goodput_bps = $(if ($timingValid) { [long]$c.goodput_bps } else { $null })
    tx_log_complete = [bool]$txConfirmed
    rx_log = (Resolve-Path -LiteralPath $InputLog).Path
    tx_log = $TxLog
}
foreach ($key in @('rssi_avg_dbm', 'rssi_min_dbm', 'rssi_max_dbm', 'fp_avg_dbm', 'fp_min_dbm', 'fp_max_dbm', 'gap_avg_db')) {
    if (-not $s.ContainsKey($key)) { throw "Missing signal field: $key" }
    $result[$key] = if ($s[$key] -eq 'NA') { $null } else { [decimal]::Parse($s[$key], [Globalization.CultureInfo]::InvariantCulture) }
    if ([long]$s.samples -gt 0 -and $null -eq $result[$key]) { throw 'Nonzero sample count with missing power statistics.' }
}
foreach ($kind in @('rssi', 'fp')) {
    if ([long]$s.samples -gt 0 -and
        ($result["${kind}_avg_dbm"] -lt $result["${kind}_min_dbm"] -or
         $result["${kind}_avg_dbm"] -gt $result["${kind}_max_dbm"])) { throw 'Power average is outside min/max bounds.' }
}
$row = [pscustomobject]$result
if ($OutputCsv) { $row | Export-Csv -LiteralPath $OutputCsv -NoTypeInformation -Encoding UTF8 }
$row
