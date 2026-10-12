[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$scripts = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$loader = Join-Path $scripts 'get_packet_target.ps1'
$reader = Join-Path $scripts 'read_no_protocol_result.ps1'
$temporary = [IO.Path]::GetTempFileName()
try {
    foreach ($number in @(1, 7, 10000, [int]::MaxValue)) {
        [IO.File]::WriteAllText($temporary, "/* shared setting */`n#define TEST_PACKET_TARGET ${number}UL // count`n")
        if ((& $loader -ConfigPath $temporary) -ne $number) { throw "Incorrect packet count: $number" }
    }
    foreach ($invalid in @('0UL', '-1', '010UL', '1.5', '(10 + 1)', '2147483648UL')) {
        [IO.File]::WriteAllText($temporary, "#define TEST_PACKET_TARGET $invalid`n")
        $rejected = $false
        try { $null = & $loader -ConfigPath $temporary } catch { $rejected = $true }
        if (-not $rejected) { throw "Invalid shared configuration accepted: $invalid" }
    }
    [IO.File]::WriteAllText($temporary, "#define TEST_PACKET_TARGET 7UL`n#define TEST_PACKET_TARGET 8UL`n")
    $rejected = $false
    try { $null = & $loader -ConfigPath $temporary } catch { $rejected = $true }
    if (-not $rejected) { throw 'Duplicate packet definitions accepted' }

    # Synthetic log verifies the result reader follows the real shared setting.
    # It is not a hardware measurement and is removed at the end of this test.
    $target = & $loader
    $samples = $target - 1
    $ppm = [math]::Floor($samples * 1000000.0 / $target)
    $power = if ($samples) { '-50.00' } else { 'NA' }
    $gap = if ($samples) { '0.00' } else { 'NA' }
    $goodput = $samples * 26L * 8L
    $log = @"
P2P 5NINES RX SIGNAL v1.1
TEST START target=$target channel=5
RX_SIGNAL_COMPLETE samples=$samples invalid=0 rssi_avg_dbm=$power rssi_min_dbm=$power rssi_max_dbm=$power fp_avg_dbm=$power fp_min_dbm=$power fp_max_dbm=$power gap_avg_db=$gap
RX_COMPLETE RESULT=FAIL target=$target end_seq=$target success=$samples failed=1 lost=1 success_ppm=$ppm dup=0 ooo=0 phy_err=0 crc_err=0 tx_elapsed_us=1000000 payload_bytes=26 goodput_bps=$goodput
"@
    [IO.File]::WriteAllText($temporary, $log)
    $result = & $reader -InputLog $temporary -ObservedSeconds 1
    if ($result.target_packets -ne $target -or $result.received_unique -ne $samples) { throw 'Reader did not use shared packet count' }
    if ($result.failed_packets -ne 1 -or $result.lost_packets -ne 1) { throw 'Failed/lost counts differ from target minus successful packets' }
    if (@($result.PSObject.Properties.Name | Where-Object { $_ -match '^invalid' }).Count) { throw 'Removed packet statistics remain in result output' }
    if ($result.signal_invalid -ne 0) { throw 'Signal diagnostic failure count was not retained' }
    [IO.File]::WriteAllText($temporary, $log.Replace('failed=1 lost=1 ', 'failed=1 lost=0 '))
    $rejected = $false
    try { $null = & $reader -InputLog $temporary } catch { $rejected = $true }
    if (-not $rejected) { throw 'Inconsistent failed/lost counts accepted' }
    if ($samples -gt 0) {
        $signalFailureLog = $log.Replace("RX_SIGNAL_COMPLETE samples=$samples invalid=0 ",
                                        "RX_SIGNAL_COMPLETE samples=$($samples - 1) invalid=1 ")
        [IO.File]::WriteAllText($temporary, $signalFailureLog)
        $result = & $reader -InputLog $temporary -ObservedSeconds 1
        if ($result.received_unique -ne $samples -or $result.failed_packets -ne 1 -or
            $result.signal_invalid -ne 1 -or $result.signal_samples -ne ($samples - 1)) {
            throw 'Signal diagnostic failure changed packet delivery statistics'
        }
    }
    [IO.File]::WriteAllText($temporary, $log.Replace("end_seq=$target ", "end_seq=$($target + 1) "))
    $rejected = $false
    try { $null = & $reader -InputLog $temporary } catch { $rejected = $true }
    if (-not $rejected) { throw 'Firmware/configuration mismatch accepted' }

    # Unknown parameters must fail during binding, before any hardware actions.
    foreach ($entry in @('repeat_no_protocol_test.ps1', 'run_no_protocol_size_tests.ps1')) {
        $arguments = if ($entry -eq 'repeat_no_protocol_test.ps1') { @{Target = 7} } else { @{Targets = 7} }
        $rejected = $false
        try { & (Join-Path $scripts $entry) @arguments } catch [Management.Automation.ParameterBindingException] { $rejected = $true }
        if (-not $rejected) { throw "Removed target override was accepted: $entry" }
    }
    Write-Output "PASS: shared configuration, simplified packet counts, signal failures, mismatch rejection, removed CLI overrides; configured target=$target"
}
finally { Remove-Item -LiteralPath $temporary -Force }
