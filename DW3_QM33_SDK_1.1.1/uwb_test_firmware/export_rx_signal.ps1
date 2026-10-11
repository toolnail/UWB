param(
    [Parameter(Mandatory)] [string] $InputLog,
    [string] $OutputCsv,
    [string] $SummaryCsv
)

$ErrorActionPreference = 'Stop'
if (-not $OutputCsv) { $OutputCsv = [IO.Path]::ChangeExtension($InputLog, 'signal.csv') }
if (-not $SummaryCsv) { $SummaryCsv = [IO.Path]::ChangeExtension($InputLog, 'signal_summary.csv') }
$culture = [Globalization.CultureInfo]::InvariantCulture

function Get-Values([string] $Line) {
    $values = @{}
    foreach ($match in [regex]::Matches($Line, '(?<key>[A-Za-z_][A-Za-z_0-9]*)=(?<value>\S+)')) {
        $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
    }
    return $values
}

function Get-Number($Values, [string] $Key) {
    if (-not $Values.ContainsKey($Key)) { throw "Missing field '$Key' in signal log" }
    if ($Values[$Key] -eq 'NA') { return $null }
    return [decimal]::Parse($Values[$Key], $culture)
}

# Stream sample rows so log_every=1 does not require keeping the run in RAM.
$sampleCount = 0
Get-Content -LiteralPath $InputLog | ForEach-Object {
    if ($_ -match '\bRX_SIGNAL\s+seq=') {
        $v = Get-Values $_
        $sampleCount++
        [pscustomobject]@{
            sequence = Get-Number $v 'seq'
            received_unique = Get-Number $v 'received'
            diag_valid = Get-Number $v 'diag_valid'
            rssi_dbm = Get-Number $v 'rssi_dbm'
            fp_power_dbm = Get-Number $v 'fp_power_dbm'
            gap_db = Get-Number $v 'gap_db'
            fp_index = Get-Number $v 'fp_index'
            fp_index_q6 = Get-Number $v 'fp_index_q6'
            f1_raw = Get-Number $v 'f1_raw'
            f2_raw = Get-Number $v 'f2_raw'
            f3_raw = Get-Number $v 'f3_raw'
            cir_power = Get-Number $v 'cir_power'
            accum_count = Get-Number $v 'accum_count'
            dgc = Get-Number $v 'dgc'
        }
    }
} | Export-Csv -LiteralPath $OutputCsv -NoTypeInformation -Encoding UTF8

$complete = Select-String -LiteralPath $InputLog -Pattern '\bRX_SIGNAL_COMPLETE\s' |
    Select-Object -Last 1 -ExpandProperty Line
if ($complete) {
    $v = Get-Values $complete
    [pscustomobject]@{
        samples = Get-Number $v 'samples'
        invalid = Get-Number $v 'invalid'
        rssi_avg_dbm = Get-Number $v 'rssi_avg_dbm'
        rssi_min_dbm = Get-Number $v 'rssi_min_dbm'
        rssi_max_dbm = Get-Number $v 'rssi_max_dbm'
        fp_avg_dbm = Get-Number $v 'fp_avg_dbm'
        fp_min_dbm = Get-Number $v 'fp_min_dbm'
        fp_max_dbm = Get-Number $v 'fp_max_dbm'
        gap_avg_db = Get-Number $v 'gap_avg_db'
        logged_samples = $sampleCount
        input_log = (Resolve-Path -LiteralPath $InputLog).Path
    } | Export-Csv -LiteralPath $SummaryCsv -NoTypeInformation -Encoding UTF8
    Write-Output "SIGNAL_SUMMARY_CSV=$SummaryCsv"
}
else {
    Write-Warning 'No RX_SIGNAL_COMPLETE found; only logged sample rows were exported.'
}
Write-Output "SIGNAL_SAMPLES=$sampleCount SIGNAL_CSV=$OutputCsv"
