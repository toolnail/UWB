[CmdletBinding()]
param(
    [switch] $BuildOnly,
    [ValidateSet('Enabled', 'Disabled')] [string] $ReceiverCrc = 'Enabled',
    [ValidateRange(1, 100)] [int] $MinimumCrcEvents = 3,
    [ValidateRange(10, 1800)] [int] $TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$platform = Join-Path $workspace 'Drivers\API\Build_Platforms\nRF52840-DK'
$projectOriginal = Join-Path $platform 'dw3000_api.emProject'
$source = Join-Path $workspace 'Drivers\API\Src\examples\ex_crc_frame_probe\crc_frame_probe.c'
$emBuild = 'C:\Program Files\SEGGER\SEGGER Embedded Studio for ARM 5.10a\bin\emBuild.exe'
$jlink = 'C:\Program Files\SEGGER\JLink_V980\JLink.exe'
$logger = 'C:\Program Files\SEGGER\JLink_V980\JLinkRTTLogger.exe'
$txSerial = '1050244938'
$rxSerial = '1050238642'
$session = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$directory = Join-Path $PSScriptRoot "crc_frame_probe\run_$session"
$null = New-Item -ItemType Directory -Path $directory -Force
Copy-Item -LiteralPath $source -Destination (Join-Path $directory 'crc_frame_probe.c')
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $directory 'run_crc_frame_probe.ps1')
$utf8 = [Text.UTF8Encoding]::new($false)
$protectedPaths = @(
    $projectOriginal,
    (Join-Path $workspace 'Drivers\API\Src\example_selection.h'),
    (Join-Path $workspace 'Drivers\API\Src\p2p_test_config.h'),
    (Join-Path $workspace 'Drivers\API\Src\config_options.c'),
    (Join-Path $workspace 'Drivers\API\Src\examples\ex_01a_simple_tx\simple_tx.c'),
    (Join-Path $workspace 'Drivers\API\Src\examples\ex_02a_simple_rx\simple_rx.c'),
    (Join-Path $workspace 'Drivers\API\Src\examples\ex_02a_simple_rx\rx_signal_metrics.h'),
    (Join-Path $platform 'Source\main.c')
    (Join-Path $platform 'Source\platform\deca_spi.c'),
    (Join-Path $workspace 'Drivers\API\Shared\dwt_uwb_driver\deca_device_api.h'),
    (Join-Path $workspace 'Drivers\API\Shared\dwt_uwb_driver\deca_compat.c'),
    (Join-Path $workspace 'Drivers\API\Shared\dwt_uwb_driver\dw3000\dw3000_device.c'),
    (Join-Path $workspace 'Drivers\API\Shared\dwt_uwb_driver\dw3000\dw3000_deca_regs.h')
)
$hashes = @{}
foreach ($path in $protectedPaths) { $hashes[$path] = (Get-FileHash -LiteralPath $path).Hash }
$txHold = $null
$txMonitor = $null
$rxMonitor = $null
$txImage = $null
$hardwareTouched = $false
$stopped = $false
$mutex = [Threading.Mutex]::new($false, "Local\UwbRepeat_${txSerial}_${rxSerial}")
$locked = $false

function Build-Probe([string] $Role) {
    $buildDirectory = Join-Path $directory "build_$Role"
    $null = New-Item -ItemType Directory -Path $buildDirectory
    [xml]$project = [IO.File]::ReadAllText($projectOriginal)
    # Rebase only this NEW project. Original SDK/platform sources are read-only.
    $platformUnix = $platform.Replace('\', '/')
    $apiUnix = (Join-Path $workspace 'Drivers\API').Replace('\', '/')
    foreach ($configuration in @($project.SelectNodes('//configuration'))) {
        foreach ($attribute in @($configuration.Attributes)) {
            $attribute.Value = $attribute.Value.Replace('$(ProjectDir)', $platformUnix)
        }
        if ($configuration.HasAttribute('macros')) {
            $configuration.SetAttribute('macros', "NordicSDKDir=$platformUnix/sdk;DW3000APIDir=$apiUnix;CMSIS_CONFIG_TOOL=`$(NordicSDKDir)/external_tools/cmsisconfig/CMSIS_Configuration_Wizard")
        }
        if ($configuration.HasAttribute('link_linker_script_file')) {
            $configuration.SetAttribute('link_linker_script_file', "$platformUnix/Setup/SEGGER_Flash.icf")
        }
        if ($configuration.HasAttribute('c_user_include_directories')) {
            $includes = $configuration.GetAttribute('c_user_include_directories').Split(';') | ForEach-Object {
                if ($_ -eq '.') { $platformUnix }
                elseif ($_ -eq './Source/platform') { "$platformUnix/Source/platform" }
                else { $_ }
            }
            $configuration.SetAttribute('c_user_include_directories', ($includes -join ';'))
        }
    }
    foreach ($file in @($project.SelectNodes('//file'))) {
        $name = $file.GetAttribute('file_name')
        if ($name -match '/Src/examples/' -and $name -notmatch '/shared_data/') {
            $null = $file.ParentNode.RemoveChild($file)
        }
        elseif (-not $name.StartsWith('$(')) {
            $file.SetAttribute('file_name', [IO.Path]::GetFullPath((Join-Path $platform $name)).Replace('\', '/'))
        }
    }
    $roleValue = if ($Role -eq 'TX') { 1 } else { 0 }
    $checkValue = if ($ReceiverCrc -eq 'Enabled') { 1 } else { 0 }
    $debug = $project.SelectSingleNode('/solution/configuration[@Name="Debug"]')
    $debug.SetAttribute('c_preprocessor_definitions', "DEBUG;CRC_PROBE_ROLE_TX=$roleValue;CRC_PROBE_RX_CHECK=$checkValue")
    $file = $project.CreateElement('file')
    $file.SetAttribute('file_name', $source.Replace('\', '/'))
    $null = $project.SelectSingleNode('/solution/project/folder[@Name="Source"]').AppendChild($file)
    $projectPath = Join-Path $buildDirectory 'crc_frame_probe.emProject'
    # Embedded Studio rejects XmlDocument.Save's empty DOCTYPE internal subset.
    [IO.File]::WriteAllText($projectPath,
        $project.OuterXml.Replace('<!DOCTYPE CrossStudio_Project_File[]>', '<!DOCTYPE CrossStudio_Project_File>'), $utf8)
    Write-Host "Building independent CRC probe $Role..."
    $process = Start-Process -FilePath $emBuild -ArgumentList @('-config', 'Debug', '-rebuild', ('"' + $projectPath + '"')) `
        -WorkingDirectory $buildDirectory -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $directory "build_$Role.log") `
        -RedirectStandardError (Join-Path $directory "build_$Role.err.log")
    if ($process.ExitCode -ne 0) { throw "CRC probe $Role build failed. See $directory\build_$Role.err.log" }
    $hex = Join-Path $directory "crc_probe_$Role.hex"
    $map = Join-Path $directory "crc_probe_$Role.map"
    Copy-Item -LiteralPath (Join-Path $buildDirectory 'Output\Debug\Exe\dw3000_api.hex') -Destination $hex
    Copy-Item -LiteralPath (Join-Path $buildDirectory 'Output\Debug\Exe\dw3000_api.map') -Destination $map
    $rtt = Select-String -LiteralPath $map -Pattern '^\s*(0x[0-9a-fA-F]+)\s+_SEGGER_RTT\s*$' | Select-Object -Last 1
    if (-not $rtt) { throw "No RTT address in $map" }
    $stopAddress = $null
    if ($Role -eq 'TX') {
        $stop = Select-String -LiteralPath $map -Pattern '^\s*(0x[0-9a-fA-F]+)\s+crc_probe_run\s*$' | Select-Object -Last 1
        if (-not $stop) { throw "No TX stop flag in $map" }
        $stopAddress = $stop.Matches[0].Groups[1].Value
    }
    [pscustomobject]@{ Hex = $hex; Map = $map; Rtt = $rtt.Matches[0].Groups[1].Value; Stop = $stopAddress }
}

function Start-Commander([string] $Serial, [string] $Label, [string[]] $Commands, [string] $Device = 'Cortex-M4') {
    $script = Join-Path $directory "$Label.jlink"
    $log = Join-Path $directory "$Label.log"
    $Commands | Set-Content -LiteralPath $script -Encoding ASCII
    $process = Start-Process -FilePath $jlink -ArgumentList @(
        '-Device', $Device, '-If', 'SWD', '-Speed', '100', '-USB', $Serial,
        '-AutoConnect', '1', '-ExitOnError', '1', '-CommanderScript', ('"' + $script + '"')
    ) -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Hidden -PassThru
    $null = $process.Handle
    [pscustomobject]@{ Process = $process; Log = $log }
}

function Close-Process($Process) {
    if ($null -eq $Process) { return }
    if (-not $Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
    $Process.Dispose()
}

function Finish-Commander($Commander, [int] $Seconds = 120) {
    if (-not $Commander.Process.WaitForExit($Seconds * 1000)) {
        Close-Process $Commander.Process
        throw "J-Link command timed out: $($Commander.Log)"
    }
    $code = $Commander.Process.ExitCode
    $Commander.Process.Dispose()
    if ($code -ne 0) { throw "J-Link command failed ($code): $($Commander.Log)" }
}

function Flash-Probe([string] $Serial, [string] $Role, [string] $Hex) {
    Write-Host "Flashing CRC probe $Role on $Serial..."
    $finish = if ($Role -eq 'TX') { 'h' } else { 'g' }
    $command = Start-Commander $Serial "flash_$Role" @('h', "loadfile $Hex", 'r', $finish, 'exit') 'nRF52840_xxAA'
    Finish-Commander $command
    if (-not (Select-String -LiteralPath $command.Log -Pattern '^O\.K\.$' -Quiet)) { throw "Flash not verified: $($command.Log)" }
}

function Start-Monitor([string] $Serial, [string] $Address, [string] $Role) {
    Start-Process -FilePath $logger -ArgumentList @(
        '-Device', 'Cortex-M4', '-If', 'SWD', '-Speed', '100', '-USB', $Serial,
        '-RTTAddress', $Address, '-RTTChannel', '0', ('"' + (Join-Path $directory "$Role.rtt.log") + '"')
    ) -RedirectStandardOutput (Join-Path $directory "$Role.monitor.log") `
      -RedirectStandardError (Join-Path $directory "$Role.monitor.err.log") -WindowStyle Hidden -PassThru
}

function Read-Fields([string] $Line) {
    $fields = @{}
    foreach ($match in [regex]::Matches($Line, '(\w+)=(\S+)')) { $fields[$match.Groups[1].Value] = $match.Groups[2].Value }
    $fields
}

function Convert-HexBytes([string] $Hex) {
    [byte[]]$bytes = for ($i = 0; $i -lt $Hex.Length; $i += 2) { [Convert]::ToByte($Hex.Substring($i, 2), 16) }
    return ,$bytes
}
function Get-Fcs([byte[]] $Bytes, [int] $Length = 32) {
    [int]$crc = 0
    for ($i = 0; $i -lt $Length; $i++) {
        $crc = $crc -bxor [int]$Bytes[$i]
        for ($bit = 0; $bit -lt 8; $bit++) {
            $crc = if ($crc -band 1) { ($crc -shr 1) -bxor 0x8408 } else { $crc -shr 1 }
        }
    }
    return $crc
}
function Get-Sequence([byte[]] $Bytes) {
    [long]$Bytes[1] + [long]$Bytes[2] * 256L + [long]$Bytes[3] * 65536L + [long]$Bytes[4] * 16777216L
}
function Get-Template([long] $Sequence) {
    [byte[]]$bytes = New-Object byte[] 32
    $bytes[0] = 0xC5
    for ($i = 0; $i -lt 4; $i++) { $bytes[1 + $i] = ($Sequence -shr ($i * 8)) -band 255 }
    for ($i = 6; $i -lt 32; $i++) { $bytes[$i] = ($Sequence + $i * 37) -band 255 }
    return ,$bytes
}
function Read-Evidence([string] $Content) {
    $pattern = '(?m)^RX_PROBE_CRC (?<fields>[^\r\n]+)\r?\nCRC_BEFORE_CLEAR hex=(?<before>[0-9A-F]{68})\r?\nCRC_AFTER_CLEAR hex=(?<after>[0-9A-F]{68})\r?\nCRC_PREVIOUS_GOOD hex=(?<previous>[0-9A-F]{68})'
    foreach ($match in [regex]::Matches($Content, $pattern)) {
        $v = Read-Fields $match.Groups['fields'].Value
        $mode = if ($v.mode) { $v.mode } else { 'ON' }
        $expectedMode = if ($ReceiverCrc -eq 'Disabled') { 'OFF' } else { 'ON' }
        if ($mode -ne $expectedMode) { throw 'Logged CRC mode differs from requested mode' }
        $checkMatch = [regex]::Match($Content.Substring($match.Index + $match.Length), "(?m)^CRC_CHECKS event=$($v.event) (?<fields>[^\r\n]+)")
        if (-not $checkMatch.Success) { continue }
        $check = Read-Fields $checkMatch.Groups['fields'].Value
        $armMatch = [regex]::Match($Content.Substring($match.Index + $match.Length), "(?m)^CRC_CHECKS event=$($v.event) [^\r\n]+\r?\nCRC_BEFORE_ARM hex=(?<arm>[0-9A-F]{68})")
        # Wait for the complete post-event baseline record in the live log.
        $firmwareHasArmBaseline = (Select-String -LiteralPath (Join-Path $directory 'crc_frame_probe.c') -Pattern 'CRC_BEFORE_ARM' -Quiet)
        if ($firmwareHasArmBaseline -and -not $armMatch.Success) { continue }
        $armHex = if ($armMatch.Success) { $armMatch.Groups['arm'].Value } else { $null }
        $sysCfg = [Convert]::ToUInt32($check.sys_cfg.Substring(2), 16)
        $statusPre = [Convert]::ToUInt32($check.status_pre.Substring(2), 16)
        $statusPost = [Convert]::ToUInt32($check.status_post.Substring(2), 16)
        $expectedConfigBits = if ($mode -eq 'OFF') { 0x4C } else { 0x48 }
        $configurationVerified = ($sysCfg -band 0x44E) -eq $expectedConfigBits
        $spiVerified = [int]$check.spi_read_errors -eq 0 -and $check.spi_write_error -eq '0'
        if (-not $configurationVerified -or -not $spiVerified) { throw 'Actual configuration or SPI integrity check failed; exclude these snapshots.' }
        if (($mode -eq 'ON' -and (-not ($statusPre -band 0x8000) -or ($statusPre -band 0x4000))) -or
            ($mode -eq 'OFF' -and -not ($statusPre -band 0x2000)) -or
            ($statusPost -band 0xE000)) {
            throw 'Completion/error state before/after clearing was not verified'
        }
        if ($v.source -ne 'NATURAL') { throw 'Error-injection log rejected by natural CRC experiment' }
        $before = $match.Groups['before'].Value
        $after = $match.Groups['after'].Value
        $previous = $match.Groups['previous'].Value
        $bytes = Convert-HexBytes $before
        $previousBytes = Convert-HexBytes $previous
        $status = [Convert]::ToUInt32($v.status.Substring(2), 16)
        if ($mode -eq 'ON' -and (-not ($status -band 0x8000) -or ($status -band 0x4000))) { throw 'CRC snapshot lacks exclusive RXFCE event' }
        if ($mode -eq 'OFF' -and -not ($status -band 0x2000)) { throw 'Disabled-CRC snapshot lacks RXFR' }
        $observedSeq = Get-Sequence $bytes
        if ($observedSeq -ne [long]$v.observed_seq) { throw 'Decoded sequence disagrees with firmware log' }
        $template = Get-Template $observedSeq
        $payloadMatches = 0; $templateDifferences = 0; $previousDifferences = 0
        for ($i = 0; $i -lt 32; $i++) {
            if ($bytes[$i] -ne $template[$i]) { $templateDifferences++ }
            if ($bytes[$i] -ne $previousBytes[$i]) { $previousDifferences++ }
            if ($i -ge 6 -and $bytes[$i] -eq $template[$i]) { $payloadMatches++ }
        }
        if ($payloadMatches -ne [int]$v.payload_matches) { throw 'Payload comparison disagrees with firmware' }
        $hasPrevious = $v.have_previous -eq '1'
        $samePrevious = $hasPrevious -and $before -eq $previous
        $sameAfterPrevious = $hasPrevious -and $after -eq $previous
        if ([int]$samePrevious -ne [int]$v.pre_previous -or [int]$sameAfterPrevious -ne [int]$v.post_previous -or
            [int]($before -eq $after) -ne [int]$v.same_reads) { throw 'Snapshot comparison disagrees with firmware' }
        $previousFcs = [int]$previousBytes[32] + [int]$previousBytes[33] * 256
        if ($hasPrevious) {
            $previousSeq = Get-Sequence $previousBytes
            $previousTemplate = Get-Template $previousSeq
            for ($i = 0; $i -lt 32; $i++) {
                if ($previousBytes[$i] -ne $previousTemplate[$i]) { throw 'Previous GOOD baseline does not match original DATA format' }
            }
            if ($previousSeq -ne [long]$v.prev_seq -or (Get-Fcs $previousBytes) -ne $previousFcs) {
                throw 'Previous successful frame sequence/FCS was not verified'
            }
        }
        $calculatedFcs = Get-Fcs $bytes
        $readFcs = [int]$bytes[32] + [int]$bytes[33] * 256
        if ($v.calculated_fcs -and ([Convert]::ToUInt32($v.calculated_fcs.Substring(2),16) -ne $calculatedFcs -or
            [Convert]::ToUInt32($v.read_fcs.Substring(2),16) -ne $readFcs)) { throw 'MCU and host CRC calculations disagree' }
        if ($mode -eq 'OFF' -and $calculatedFcs -eq $readFcs) { throw 'Disabled-CRC snapshot is not a software CRC mismatch' }
        $followingPattern = "(?m)^RX_PROBE_GOOD seq=(\d+) [^\r\n]*verified=1 after_crc_event=$($v.event)(?:\r?$)"
        $following = [regex]::Match($Content.Substring($match.Index + $match.Length), $followingPattern)
        $followingSeq = if ($following.Success) { [long]$following.Groups[1].Value } else { $null }
        $sequenceBracket = $hasPrevious -and $null -ne $followingSeq -and
            $observedSeq -gt [long]$v.prev_seq -and $observedSeq -lt $followingSeq
        # A changed buffer alone cannot establish an intact current frame ID.
        # Require original header, supported payload identity, a CRC mismatch,
        # and the observed sequence bracketed by successful receptions.
        $current = $hasPrevious -and -not $samePrevious -and [int]$v.frame_len -eq 34 -and
            $bytes[0] -eq 0xC5 -and $bytes[5] -eq 0 -and $payloadMatches -ge 24 -and
            $sequenceBracket -and $calculatedFcs -ne $readFcs -and $before -eq $after
        # Buffer freshness and recovery of the original TX frame ID are two
        # different conclusions. Natural errors may destroy the sequence/type.
        $updatedErrorData = $hasPrevious -and -not $samePrevious -and [int]$v.frame_len -eq 34 -and
            $calculatedFcs -ne $readFcs -and $before -eq $after -and $null -ne $followingSeq
        $classification = if ($samePrevious) { 'MATCHES_PREVIOUS_FRAME' }
            elseif ($null -ne $armHex -and $before -ne $armHex) { 'BUFFER_UPDATED_DURING_CURRENT_RX_ATTEMPT' }
            elseif ($current) { 'CURRENT_FRAME_CANDIDATE_UNCONFIRMED' }
            elseif ($updatedErrorData) { 'BUFFER_CHANGED_SOURCE_UNCONFIRMED' }
            elseif ($hasPrevious) { 'UPDATED_ID_UNCERTAIN' } else { 'NO_GOOD_BASELINE' }
        [pscustomobject]@{
            event = [int]$v.event; source = 'NATURAL'; classification = $classification
            receiver_crc = $mode; detection_cause = $v.cause
            hardware_rxfce = [bool]($status -band 0x8000); hardware_rxfcg = [bool]($status -band 0x4000)
            status = $v.status; reported_frame_length = [int]$v.frame_len
            sys_cfg = $check.sys_cfg; rx_finfo = $check.rx_finfo; sys_state = $check.sys_state
            status_before_clear = $check.status_pre; status_after_clear = $check.status_post
            configuration_verified = $configurationVerified; spi_integrity_verified = $spiVerified
            spi_read_errors = [int]$check.spi_read_errors; repeat_before_clear_equal = $check.repeat_equal -eq '1'
            before_arm_hex = $armHex
            changed_during_this_rx_attempt = $null -ne $armHex -and $before -ne $armHex
            arm_baseline_matches_previous_good = $hasPrevious -and $null -ne $armHex -and $armHex -eq $previous
            previous_good_seq = [long]$v.prev_seq; observed_seq = $observedSeq; following_good_seq = $followingSeq
            current_identity_supported = $current; exact_previous_frame = $samePrevious
            updated_error_snapshot = $updatedErrorData
            buffer_changed = $hasPrevious -and -not $samePrevious; same_reads = $before -eq $after
            payload_matches_of_26 = $payloadMatches; body_diff_bytes_from_template = $templateDifferences
            body_diff_bytes_from_previous = $previousDifferences
            calculated_fcs = $calculatedFcs.ToString('X4'); read_fcs = $readFcs.ToString('X4')
            read_frame_crc_mismatch = $calculatedFcs -ne $readFcs
            successful_frames_before_event = [long]$v.good; other_error_events = [long]$v.other
            before_hex = $before; after_hex = $after; previous_good_hex = $previous
        }
    }
}

function Stop-Transmitter {
    if (-not $hardwareTouched -or $null -eq $txImage) { return }
    Close-Process $txMonitor
    $script:txMonitor = $null
    Write-Host 'Stopping the continuous transmitter using its RAM stop flag...'
    $command = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $command = Start-Commander $txSerial "stop_TX_$attempt" @('h', "w4 $($txImage.Stop), 0", 'g', 'sleep 1000', 'h', "mem32 $($txImage.Stop), 1", 'g', 'exit')
        try { Finish-Commander $command 30; break }
        catch { if ($attempt -eq 3) { throw }; Write-Host "Retry TX stop connection $attempt/3..." }
    }
    $stopAddressText = ([Convert]::ToUInt32($txImage.Stop.Substring(2), 16)).ToString('X8')
    if (-not (Select-String -LiteralPath $command.Log -Pattern "^$stopAddressText = 00000000" -Quiet)) {
        throw "TX stop flag readback was not confirmed: $($command.Log)"
    }
    $script:stopped = $true
}

try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Another test owns these probes.' }
    Write-Host "CRC experiment directory: $directory"
    $rxImage = Build-Probe 'RX'
    $txImage = Build-Probe 'TX'
    if ($BuildOnly) {
        Write-Host "BUILD_ONLY RX=$($rxImage.Hex) TX=$($txImage.Hex)"
        return
    }
    foreach ($required in @($jlink, $logger)) { if (-not (Test-Path -LiteralPath $required)) { throw "Missing tool: $required" } }
    if (Get-Process -Name JLink,JLinkRTTLogger -ErrorAction SilentlyContinue) { throw 'Close existing J-Link/RTT sessions before running this independent experiment.' }
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $halt = Start-Commander $txSerial "halt_previous_TX_$attempt" @('h', 'exit')
        try { Finish-Commander $halt 30; break }
        catch { if ($attempt -eq 3) { throw }; Write-Host "Retry TX connection $attempt/3..." }
    }
    Flash-Probe $txSerial 'TX' $txImage.Hex
    $hardwareTouched = $true
    $held = $false
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $txHold = Start-Commander $txSerial "hold_TX_$attempt" @('h', 'r', 'h', 'regs', 'regs', 'regs', 'sleep 7200000', 'exit')
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline) {
            if (Select-String -LiteralPath $txHold.Log -Pattern 'Reset: Reset device via AIRCR.SYSRESETREQ.' -Quiet -ErrorAction SilentlyContinue) { $held = $true; break }
            if ($txHold.Process.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($held) { break }
        Close-Process $txHold.Process
        $txHold = $null
        Write-Host "TX reset/hold connection retry $attempt/3..."
    }
    if (-not $held) { throw 'TX reset/hold was not confirmed' }
    Flash-Probe $rxSerial 'RX' $rxImage.Hex
    $rxMonitor = Start-Monitor $rxSerial $rxImage.Rtt 'RX'
    $rxLog = Join-Path $directory 'RX.rtt.log'
    $deadline = (Get-Date).AddSeconds(45)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        if ($rxMonitor.HasExited) { throw 'RX monitor exited' }
        if ((Test-Path -LiteralPath $rxLog) -and (Select-String -LiteralPath $rxLog -Pattern 'INIT_FAILED' -Quiet)) { throw "RX initialization failed: $rxLog" }
        if ((Test-Path -LiteralPath $rxLog) -and (Select-String -LiteralPath $rxLog -Pattern '^NATURAL_CRC_RX READY ' -Quiet)) { $ready = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw "CRC RX did not become ready: $rxLog" }
    Close-Process $txHold.Process
    $txHold = $null
    $txMonitor = Start-Monitor $txSerial $txImage.Rtt 'TX'
    Write-Host "RX ready: ReceiverCrc=$ReceiverCrc. TX sends ordinary DATA with hardware FCS and 500 us delay; waiting up to $TimeoutSeconds seconds for natural damaged frames."
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $nextUpdate = Get-Date
    $evidence = @()
    $finished = $false
    while ((Get-Date) -lt $deadline) {
        if ($rxMonitor.HasExited -or $txMonitor.HasExited) { throw 'RTT monitor closed during the experiment' }
        $content = Get-Content -LiteralPath $rxLog -Raw
        $evidence = @(Read-Evidence $content)
        $classified = @($evidence | Where-Object { $_.updated_error_snapshot -or $_.exact_previous_frame })
        # Stop after collecting a snapshot, not after claiming that an RXFCE
        # payload was recovered. Raw-buffer changes do not establish ownership.
        if (@($classified | Where-Object { $null -ne $_.following_good_seq }).Count -ge $MinimumCrcEvents) { $finished = $true; break }
        if ((Get-Date) -ge $nextUpdate) {
            $progress = [regex]::Matches($content, '(?m)^RX_PROBE_PROGRESS [^\r\n]+') | Select-Object -Last 1
            if ($progress) { Write-Host $progress.Value }
            else { Write-Host 'Waiting for natural CRC errors...' }
            $nextUpdate = (Get-Date).AddSeconds(15)
        }
        Start-Sleep -Milliseconds 250
    }
    Stop-Transmitter
    Start-Sleep -Milliseconds 300
    Close-Process $rxMonitor
    $rxMonitor = $null
    $evidence = @(Read-Evidence ([IO.File]::ReadAllText($rxLog)))
    if ($evidence.Count) { $evidence | Export-Csv -LiteralPath (Join-Path $directory 'crc_evidence.csv') -NoTypeInformation -Encoding UTF8 }
    $summary = [pscustomobject]@{
        experiment = 'NATURAL_CRC'; tx_hardware_fcs = $true; error_injection = $false
        receiver_crc = $ReceiverCrc
        hardware_rxfce_events = @($evidence | Where-Object hardware_rxfce).Count
        software_crc_mismatch_events = @($evidence | Where-Object { $_.detection_cause -eq 'SOFTWARE_CRC_MISMATCH' }).Count
        tx_data_format = 'original_simple_tx_DATA'; tx_interval_us = 500; no_packet_target = $true
        diagnostic_snapshot_obtained = $finished; reliable_error_payload_interface_established = $false
        observation_limit_seconds = $TimeoutSeconds
        crc_events_with_complete_snapshots = $evidence.Count
        minimum_crc_events = $MinimumCrcEvents
        events_with_verified_configuration_and_spi = @($evidence | Where-Object { $_.configuration_verified -and $_.spi_integrity_verified }).Count
        three_reads_identical = @($evidence | Where-Object { $_.same_reads -and $_.repeat_before_clear_equal }).Count
        direct_before_arm_snapshots = @($evidence | Where-Object { $null -ne $_.before_arm_hex }).Count
        changed_during_single_rx_attempt = @($evidence | Where-Object changed_during_this_rx_attempt).Count
        arm_baseline_equals_previous_good = @($evidence | Where-Object arm_baseline_matches_previous_good).Count
        current_frame_candidates_unconfirmed = @($evidence | Where-Object current_identity_supported).Count
        changed_buffer_source_unconfirmed = @($evidence | Where-Object { $_.classification -eq 'BUFFER_CHANGED_SOURCE_UNCONFIRMED' }).Count
        exact_previous_frame = @($evidence | Where-Object exact_previous_frame).Count
        updated_identity_uncertain = @($evidence | Where-Object { $_.classification -eq 'UPDATED_ID_UNCERTAIN' }).Count
        snapshots_with_crc_mismatch = @($evidence | Where-Object read_frame_crc_mismatch).Count
        unchanged_by_clearing_status = @($evidence | Where-Object same_reads).Count
        tx_stopped = $stopped; rx_log = $rxLog
    }
    $summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'summary.json') -Encoding UTF8
    $summary | Format-List | Out-Host
    if (-not $finished) { Write-Warning 'Observation ended without a conclusively classified natural CRC event. This is NOT evidence that CRC-error data cannot be read.' }
    Write-Host "CRC_EVIDENCE_DIRECTORY=$directory"

}
finally {
    if ($null -ne $txHold) { Close-Process $txHold.Process; $txHold = $null }
    if ($hardwareTouched -and -not $stopped) { try { Stop-Transmitter } catch { Write-Warning "Could not stop CRC TX: $_" } }
    Close-Process $txMonitor
    Close-Process $rxMonitor
    $originalChecks = @(foreach ($path in $protectedPaths) {
        $afterHash = (Get-FileHash -LiteralPath $path).Hash
        [pscustomobject]@{ path = $path; before_sha256 = $hashes[$path]; after_sha256 = $afterHash; unchanged = $afterHash -eq $hashes[$path] }
    })
    $originalChecks | Export-Csv -LiteralPath (Join-Path $directory 'original_files_check.csv') -NoTypeInformation -Encoding UTF8
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    if (@($originalChecks | Where-Object { -not $_.unchanged }).Count) { throw 'An original project file changed during the independent experiment. See original_files_check.csv' }
}
