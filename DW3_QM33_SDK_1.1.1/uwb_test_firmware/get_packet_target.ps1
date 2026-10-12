param(
    [string] $ConfigPath = (Join-Path $PSScriptRoot '..\Drivers\API\Src\p2p_test_config.h')
)

$ErrorActionPreference = 'Stop'
$content = [IO.File]::ReadAllText($ConfigPath)
# Strip comments before looking for a single, unconditional decimal definition.
$content = [regex]::Replace($content, '(?s)/\*.*?\*/', ' ')
$content = [regex]::Replace($content, '(?m)//[^\r\n]*', '')
$definitions = [regex]::Matches($content, '(?m)^[ \t]*#[ \t]*define[ \t]+TEST_PACKET_TARGET\b[^\r\n]*')
if ($definitions.Count -ne 1) { throw "Expected exactly one TEST_PACKET_TARGET definition in $ConfigPath" }
$definition = [regex]::Match($definitions[0].Value,
    '^[ \t]*#[ \t]*define[ \t]+TEST_PACKET_TARGET[ \t]+(?<number>[1-9][0-9]*)(?:[uU][lL]?|[lL][uU]?)?[ \t]*$')
if (-not $definition.Success) { throw "TEST_PACKET_TARGET must be a positive decimal integer such as 10000UL in $ConfigPath" }
$target = [long]::Parse($definition.Groups['number'].Value, [Globalization.CultureInfo]::InvariantCulture)
if ($target -gt [int]::MaxValue) { throw "TEST_PACKET_TARGET exceeds 2147483647 in $ConfigPath" }
return [int]$target
