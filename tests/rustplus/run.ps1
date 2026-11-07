# MTG "Rust+" acceptance: error-code handbook, precise E0277 diagnostics,
# automatic '?' propagation trace, and non-zero exit code for Main::main.
# ASCII-only on purpose: Windows PowerShell 5.1 reads BOM-less .ps1 as ANSI.
param(
    [string]$Compiler = (Join-Path $PSScriptRoot '..\..\mtg.exe')
)

$ErrorActionPreference = 'Continue'
$mtg = (Resolve-Path $Compiler).Path
$dir = $PSScriptRoot
$tmp = Join-Path $env:TEMP 'mtg_rustplus'
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

Write-Host '[1] built-in error-code handbook'
$codes = (& $mtg --list-codes 2>&1 | Out-String)
Check ($codes -match 'E0277') '--list-codes includes E0277' ''
$explain = (& $mtg explain E0277 2>&1 | Out-String)
Check ($explain -match 'Main::main\(\)\.Result') 'explain E0277 shows a compilable fix' ''
Check ($explain -match 'Result-returning') 'explain E0277 has an English title' ''

Write-Host '[2] Ok path (no error)'
& $mtg build (Join-Path $dir 'ok.wei') -o (Join-Path $tmp 'ok.exe') | Out-Null
$okBuild = $LASTEXITCODE
Check ($okBuild -eq 0) 'ok.wei builds' "build exit=$okBuild"
$okOut = (& (Join-Path $tmp 'ok.exe') 2>&1 | Out-String)
$okCode = $LASTEXITCODE
Check ($okCode -eq 0) 'Ok path exit code is 0' "exit=$okCode"
Check ($okOut.Trim() -eq '5') 'Ok path prints 5' "out=[$($okOut.Trim())]"
Check ($okOut -notmatch 'Math::div') 'Ok path prints no trace line' ''

Write-Host '[3] Err path (two-level propagation trace)'
& $mtg build (Join-Path $dir 'trace.wei') -o (Join-Path $tmp 'trace.exe') | Out-Null
$trBuild = $LASTEXITCODE
Check ($trBuild -eq 0) 'trace.wei builds' "build exit=$trBuild"
$trOut = (& (Join-Path $tmp 'trace.exe') 2>&1 | Out-String)
$trCode = $LASTEXITCODE
Check ($trCode -eq 7) 'Err exit code equals abs(error code) = 7' "exit=$trCode"
Check ($trOut -match 'Math::safe') 'trace contains inner site Math::safe' "out=[$($trOut.Trim())]"
Check ($trOut -match 'Main::main') 'trace contains outer site Main::main' ''
$sites = @($trOut -split "`n" | Where-Object { $_ -match 'Math::' })
Check ($sites.Count -eq 2) 'trace has exactly two sites' "count=$($sites.Count)"

Write-Host '[4] -no-error-trace (zero-cost off switch)'
& $mtg build (Join-Path $dir 'trace.wei') -o (Join-Path $tmp 'notrace.exe') -no-error-trace | Out-Null
$noBuild = $LASTEXITCODE
Check ($noBuild -eq 0) 'build with -no-error-trace succeeds' "build exit=$noBuild"
$noOut = (& (Join-Path $tmp 'notrace.exe') 2>&1 | Out-String)
$noCode = $LASTEXITCODE
Check ($noCode -eq 7) 'exit code still 7 with trace disabled' "exit=$noCode"
Check ($noOut -notmatch 'Math::safe') 'no trace lines when disabled' "out=[$($noOut.Trim())]"

Write-Host '[5] precise E0277 diagnostic'
$eOut = (& $mtg build (Join-Path $dir 'e0277.wei') -o (Join-Path $tmp 'e0277.exe') 2>&1 | Out-String)
$eCode = $LASTEXITCODE
Check ($eCode -ne 0) 'e0277.wei is rejected' "exit=$eCode"
Check ($eOut -match 'error\[E0277\]') 'diagnostic carries error code E0277' "out=[$($eOut.Trim())]"
Check ($eOut -match 'explain E0277') 'diagnostic points at the handbook' ''
Check ($eOut -match '\^') 'diagnostic underlines the ? token' ''

Write-Host ''
if ($fail -eq 0) {
    Write-Host 'rustplus: ALL PASS' -ForegroundColor Green
    exit 0
} else {
    Write-Host "rustplus: $fail FAILED" -ForegroundColor Red
    exit 1
}
