param(
    [string]$Compiler = "",
    [string[]]$Only = @()
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

$cases = @(
    "hello", "arith", "array", "bytes", "control",
    "cmdline", "exit", "file", "func", "string"
)
if ($Only.Count -gt 0) {
    $cases = @($cases | Where-Object { $Only -contains $_ })
}

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

function Normalize([string]$Text) {
    if ($null -eq $Text) { return "" }
    return ($Text -replace "`r`n", "`n").TrimEnd("`r", "`n")
}

$stamp = [guid]::NewGuid().ToString("N")
$work = Join-Path ([System.IO.Path]::GetTempPath()) "mtg-bootstrap-accept-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$passed = 0
$failed = 0
$details = New-Object System.Collections.Generic.List[string]

foreach ($case in $cases) {
    $src = Join-Path $here "$case.wei"
    $exe = Join-Path $work "$case.exe"
    $build = Invoke-Captured -File $Compiler `
        -Arguments ("build `"$src`" -o `"$exe`"") `
        -WorkingDirectory $work

    if ($build.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        $failed++
        $details.Add("$case : compile exit $($build.ExitCode)")
        $out = Normalize $build.StdOut
        $err = Normalize $build.StdErr
        if ($out) { $details.Add("stdout: $out") }
        if ($err) { $details.Add("stderr: $err") }
        continue
    }

    $runDir = Join-Path $work "run-$case"
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $arguments = ""
    $argsFile = Join-Path $here "$case.args"
    if (Test-Path -LiteralPath $argsFile) {
        $arguments = [System.IO.File]::ReadAllText($argsFile)
        $arguments = ($arguments -replace "`r`n", "`n").TrimEnd("`r", "`n")
    }

    $run = Invoke-Captured -File $exe -Arguments $arguments -WorkingDirectory $runDir

    $expected = [System.IO.File]::ReadAllText((Join-Path $here "$case.expected"))
    $expected = ($expected -replace "`r`n", "`n").TrimEnd("`r", "`n")
    $actual = Normalize $run.StdOut

    $expectedExit = 0
    $exitFile = Join-Path $here "$case.exit"
    if (Test-Path -LiteralPath $exitFile) {
        $expectedExit = [int](([System.IO.File]::ReadAllText($exitFile)).Trim())
    }

    if (($actual -ceq $expected) -and ($run.ExitCode -eq $expectedExit)) {
        $passed++
        Write-Host "PASS $case"
    } else {
        $failed++
        if ($run.ExitCode -ne $expectedExit) {
            $details.Add("$case : exit $($run.ExitCode), expected $expectedExit")
        }
        if (-not ($actual -ceq $expected)) {
            $details.Add("$case : output mismatch")
            $details.Add("expected:")
            $details.Add($expected)
            $details.Add("actual:")
            $details.Add($actual)
        }
        $err = Normalize $run.StdErr
        if ($err) { $details.Add("$case stderr: $err") }
    }
}

Write-Host ""
Write-Host "bootstrap acceptance: $passed passed, $failed failed"
foreach ($line in $details) {
    Write-Host "---"
    Write-Host $line
}

Remove-Item -Recurse -Force -Path $work -ErrorAction SilentlyContinue

if ($failed -gt 0) { exit 1 }
exit 0
