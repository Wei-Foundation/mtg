# Regression: one line-break marker (outlb / newline / <CJK>) must emit
# exactly one line break. do.emit:: already appends one newline per value,
# so a marker carrying its own "\n" used to produce a blank line twice.
# ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI.
param(
    [string]$Compiler = (Join-Path $PSScriptRoot '..\..\mtg.exe')
)

$ErrorActionPreference = 'Continue'
$mtg = (Resolve-Path $Compiler).Path
$tmp = Join-Path $env:TEMP 'mtg_emit'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$fail = 0

function Check([bool]$ok, [string]$name, [string]$detail) {
    if ($ok) {
        Write-Host ('  [PASS] ' + $name)
    } else {
        Write-Host ('  [FAIL] ' + $name + '  ' + $detail) -ForegroundColor Red
        $script:fail++
    }
}

Write-Host '[1] build outlb.wei'
$exe = Join-Path $tmp 'outlb.exe'
& $mtg build (Join-Path $PSScriptRoot 'outlb.wei') -o $exe | Out-Null
$buildCode = $LASTEXITCODE
Check ($buildCode -eq 0) 'outlb.wei builds' "build exit=$buildCode"

if ($buildCode -eq 0) {
    Write-Host '[2] raw stdout byte check'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $text = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $flat = $text -replace "`r`n", '|'
    $lf = ([regex]::Matches($text, "`n")).Count

    # A<NL> outlb<NL> B<NL> newline<NL> C<NL>  ->  A||B||C|  (5 CRLF)
    # Before the fix each marker emitted 2 -> A|||B|||C| (7 CRLF).
    Check ($flat -eq 'A||B||C|') 'one marker emits exactly one line break' "flat=[$flat]"
    Check ($lf -eq 5) 'total line breaks = 5' "lf=$lf"
}

if ($fail -eq 0) {
    Write-Host 'emit regression: PASS'
} else {
    Write-Host ("emit regression: FAIL ($fail)") -ForegroundColor Red
}
exit $fail
