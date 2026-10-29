param(
    [string]$Compiler = "",
    [switch]$RequireBuildSuccess
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

function Normalize([string]$Text) {
    if ($null -eq $Text) { return "" }
    return ($Text -replace "`r`n", "`n").TrimEnd("`r", "`n")
}

$stamp = [guid]::NewGuid().ToString("N")
$work = Join-Path ([System.IO.Path]::GetTempPath()) "mtg-bootstrap-cli-$stamp"
New-Item -ItemType Directory -Force -Path $work | Out-Null

$hello = Join-Path $here "hello.wei"
$outName = "hello-cli.exe"
$outExe = Join-Path $work $outName
$defaultDir = Join-Path $work "default-out"
New-Item -ItemType Directory -Force -Path $defaultDir | Out-Null
$badSrc = Join-Path $work "bad.wei"
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($badSrc, "create.mod Main::main() { do.emit::; }", $utf8)

$passed = 0
$failed = 0

function Check {
    param([string]$Name, [bool]$Condition)
    if ($Condition) {
        $script:passed++
        Write-Host "PASS $Name"
    } else {
        $script:failed++
        Write-Host "FAIL $Name"
    }
}

function Build-Hello {
    param([string]$Extra)
    $arguments = "build `"$hello`" -o `"$outName`""
    if ($Extra) { $arguments = "$arguments $Extra" }
    return Invoke-Captured -File $Compiler -Arguments $arguments -WorkingDirectory $work
}

$r = Invoke-Captured -File $Compiler -Arguments "--version" -WorkingDirectory $work
Check "version-exit" ($r.ExitCode -eq 0)
Check "version-text" ((Normalize $r.StdOut).Contains("Module Target Generator"))

$r = Invoke-Captured -File $Compiler -Arguments "" -WorkingDirectory $work
Check "noargs-exit" ($r.ExitCode -eq 0)
Check "noargs-banner" ((Normalize $r.StdOut).Contains("Module Target Generator"))

$r = Invoke-Captured -File $Compiler -Arguments "build" -WorkingDirectory $work
Check "build-no-file" ($r.ExitCode -eq 2)

$missing = Join-Path $work "missing.wei"
$r = Invoke-Captured -File $Compiler -Arguments "build `"$missing`"" -WorkingDirectory $work
Check "missing-source" ($r.ExitCode -eq 1)

$r = Invoke-Captured -File $Compiler -Arguments "frobnicate x" -WorkingDirectory $work
Check "unknown-subcommand" ($r.ExitCode -eq 2)

$r = Build-Hello ""
Check "build-success" ($r.ExitCode -eq 0)
Check "build-done-text" ((Normalize $r.StdOut).Contains("[done] hello-cli.exe (win64)"))
Check "build-output-exists" (Test-Path -LiteralPath $outExe)

$run = Invoke-Captured -File $outExe -Arguments "" -WorkingDirectory $work
Check "built-program-output" ((Normalize $run.StdOut) -ceq "Hello, bootstrap!")

$r = Invoke-Captured -File $Compiler -Arguments ("build `"$hello`"") -WorkingDirectory $defaultDir
Check "default-output-name" (($r.ExitCode -eq 0) -and (Test-Path -LiteralPath (Join-Path $defaultDir "hello.exe")))

$r = Build-Hello "-std:zh"
Check "std-zh-rejected" ($r.ExitCode -eq 2)

$r = Build-Hello "-std:wenyan"
Check "std-wenyan-rejected" ($r.ExitCode -eq 2)

$r = Build-Hello "-std:wei27"
Check "std-wei27-accepted" ($r.ExitCode -eq 0)

$r = Build-Hello "-target linux"
Check "target-linux-rejected" ($r.ExitCode -eq 2)

$r = Build-Hello "-target win64"
Check "target-win64-accepted" ($r.ExitCode -eq 0)

$r = Build-Hello "-wide"
Check "wide-rejected" ($r.ExitCode -eq 2)

$r = Invoke-Captured -File $Compiler -Arguments ("build `"$hello`" -o") -WorkingDirectory $work
Check "o-missing-value" ($r.ExitCode -eq 2)

$r = Invoke-Captured -File $Compiler -Arguments ("build `"$badSrc`" -o `"$(Join-Path $work 'bad.exe')`"") -WorkingDirectory $work
Check "syntax-error" ($r.ExitCode -eq 1)

if ($RequireBuildSuccess) {
    $bootstrapSrc = Join-Path $repo "src\bootstrap.wei"
    $bootstrapExe = Join-Path $work "bootstrap-cli.exe"
    $r = Invoke-Captured -File $Compiler `
        -Arguments ("build `"$bootstrapSrc`" -o `"$bootstrapExe`"") `
        -WorkingDirectory $work
    Check "bootstrap-rebuild" (($r.ExitCode -eq 0) -and (Test-Path -LiteralPath $bootstrapExe))
}

Write-Host ""
Write-Host "bootstrap CLI acceptance: $passed passed, $failed failed"

Remove-Item -Recurse -Force -Path $work -ErrorAction SilentlyContinue

if ($failed -gt 0) { exit 1 }
exit 0
