param(
    [string]$Compiler = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)

if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = Join-Path $repo "mtg.exe"
}
if (-not (Test-Path -LiteralPath $Compiler)) {
    Write-Host "compiler not found: $Compiler"
    exit 1
}
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

function Invoke-Captured {
    param(
        [string]$File,
        [string]$Arguments,
        [string]$WorkingDirectory
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = $Arguments
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $p.ExitCode
        StdOut = $stdout
        StdErr = $stderr
    }
}

function Get-Sha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $bytes = $sha.ComputeHash($stream)
        return ([BitConverter]::ToString($bytes)).Replace("-", "")
    } finally {
        $stream.Close()
        $sha.Dispose()
    }
}

$stamp = [guid]::NewGuid().ToString("N")
$work = Join-Path ([System.IO.Path]::GetTempPath()) "mtg-bootstrap-fixpoint-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$src = Join-Path $repo "src\bootstrap.wei"
$stage1 = Join-Path $work "stage1.exe"
$stage2 = Join-Path $work "stage2.exe"
$stage3 = Join-Path $work "stage3.exe"

$failed = 0

$r = Invoke-Captured -File $Compiler `
    -Arguments ("build `"$src`" -o `"$stage1`"") `
    -WorkingDirectory $work
if ($r.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $stage1)) {
    Write-Host "FAIL stage1: $Compiler could not build src/bootstrap.wei"
    Write-Host ($r.StdOut + $r.StdErr)
    $failed++
}

if ($failed -eq 0) {
    $r = Invoke-Captured -File $stage1 `
        -Arguments ("build `"$src`" -o `"$stage2`"") `
        -WorkingDirectory $work
    if ($r.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $stage2)) {
        Write-Host "FAIL stage2: stage1 could not rebuild src/bootstrap.wei"
        Write-Host ($r.StdOut + $r.StdErr)
        $failed++
    }
}

if ($failed -eq 0) {
    $r = Invoke-Captured -File $stage2 `
        -Arguments ("build `"$src`" -o `"$stage3`"") `
        -WorkingDirectory $work
    if ($r.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $stage3)) {
        Write-Host "FAIL stage3: stage2 could not rebuild src/bootstrap.wei"
        Write-Host ($r.StdOut + $r.StdErr)
        $failed++
    }
}

if ($failed -eq 0) {
    $h2 = Get-Sha256 $stage2
    $h3 = Get-Sha256 $stage3
    Write-Host "stage2: $h2"
    Write-Host "stage3: $h3"
    if ($h2 -ceq $h3) {
        Write-Host "PASS fixpoint: stage2 and stage3 are byte-identical"
    } else {
        Write-Host "FAIL fixpoint: stage2 and stage3 differ"
        $failed++
    }
}

Remove-Item -Recurse -Force -Path $work -ErrorAction SilentlyContinue

Write-Host ""
if ($failed -gt 0) {
    Write-Host "bootstrap fixpoint: FAILED"
    exit 1
}
Write-Host "bootstrap fixpoint: OK"
exit 0
