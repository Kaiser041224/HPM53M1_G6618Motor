# Build and run the YHorizon-JM FOC algorithm off-line verification suite.
#
#   .\build_and_run.ps1
#   .\build_and_run.ps1 -Gcc C:\path\to\gcc.exe
#   .\build_and_run.ps1 -Keep        # keep the generated executable
#
# Host gcc only (MinGW-w64); no ARM toolchain required.
# Exit code: 0 = all checks passed; non-zero = build failure or a failed assertion.
#
# NOTE: this script is deliberately ASCII-only. Windows PowerShell reads .ps1 files
# using the ANSI code page unless they carry a UTF-8 BOM, so non-ASCII literals here
# would break parsing. The C sources keep Chinese comments (UTF-8, no BOM) and the
# console is switched to UTF-8 below so their output renders correctly.

param(
    [string]$Gcc = '',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'

# Make Chinese output from the test binary render correctly in the Windows console.
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {
    # Non-interactive host without a console: ignore.
}

$HostTestDir = $PSScriptRoot
$RefSrc      = Join-Path (Split-Path $HostTestDir -Parent) 'reference_src'
$FocDir      = Join-Path $RefSrc 'Foc'
$AppDir      = Join-Path $RefSrc 'App'
$StubsDir    = Join-Path $HostTestDir 'stubs'
$OutExe      = Join-Path $HostTestDir 'foc_algorithm_test.exe'

function Resolve-Gcc {
    param([string]$Explicit)

    if ($Explicit -ne '') {
        if (Test-Path -LiteralPath $Explicit) { return $Explicit }
        throw "Specified gcc not found: $Explicit"
    }
    $cmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in @(
        'C:\msys64\mingw64\bin\gcc.exe',
        'D:\MingW64\mingw64\bin\gcc.exe',
        'C:\mingw64\bin\gcc.exe',
        'C:\ProgramData\mingw64\mingw64\bin\gcc.exe')) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "Host gcc not found. Install MinGW-w64, or pass -Gcc <path>."
}

foreach ($required in @($FocDir, $AppDir, $StubsDir)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing directory: $required (incomplete reference_src?)"
    }
}

$gccExe = Resolve-Gcc -Explicit $Gcc

$sources = @(
    (Join-Path $HostTestDir 'test_foc_algorithm.c'),
    (Join-Path $FocDir 'foc_math.c'),
    (Join-Path $FocDir 'foc_svpwm.c'),
    (Join-Path $FocDir 'foc_vernier.c'),
    (Join-Path $FocDir 'foc_current.c')
)

# stubs must come first: foc_vernier.c does #include "encoder.h" and must hit the stub.
$includeArgs = @(
    "-I$StubsDir",
    "-I$FocDir",
    "-I$AppDir"
)

Write-Host "gcc        : $gccExe"
Write-Host "reference  : $RefSrc"
Write-Host ""

& $gccExe -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter `
    @includeArgs @sources -o $OutExe -lm

if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE)"
}

Write-Host "--- run ---"
& $OutExe
$testExit = $LASTEXITCODE
Write-Host "--- done (exit $testExit) ---"

if (-not $Keep) {
    Remove-Item -LiteralPath $OutExe -ErrorAction SilentlyContinue
}

if ($testExit -ne 0) {
    throw "Verification failed; see FAIL lines above."
}

Write-Host "All checks passed."
exit 0
