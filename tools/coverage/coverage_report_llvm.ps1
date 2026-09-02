# coverage_report_llvm.ps1 - LLVM source-based coverage terminal summary for Aurora (no HTML).
# Invoked by the CMake `coverage` target when the toolchain is Clang. Prereqs:
#   * targets built with -fprofile-instr-generate -fcoverage-mapping
#   * ctest run with LLVM_PROFILE_FILE=<BuildDir>/profraw/aurora-%p.profraw (per-pid, no clobber)
# Pipeline: llvm-profdata merge *.profraw -> aurora.profdata; llvm-cov report over test/tool
# executables (instrumented binaries carry the coverage mapping) filtered to src/ + include/.
param(
    [string]$BuildDir = $PWD,
    [string]$SrcRoot  = (Split-Path -Parent $BuildDir),
    [string]$LlvmBin  = ""
)

$ErrorActionPreference = 'Continue'

function Find-Tool([string]$name) {
    if ($LlvmBin -and (Test-Path (Join-Path $LlvmBin "$name.exe"))) { return (Join-Path $LlvmBin "$name.exe") }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$profdata = Find-Tool 'llvm-profdata'
$llvmcov  = Find-Tool 'llvm-cov'
if (-not $profdata -or -not $llvmcov) {
    Write-Host 'ERROR: llvm-profdata / llvm-cov not found (pass -LlvmBin or add LLVM bin to PATH).'
    exit 1
}

$raws = Get-ChildItem -Recurse -Path (Join-Path $BuildDir 'profraw') -Filter *.profraw -ErrorAction SilentlyContinue
if (-not $raws -or $raws.Count -eq 0) {
    Write-Host 'ERROR: no .profraw found. Run tests via the coverage target (sets LLVM_PROFILE_FILE), e.g.:'
    Write-Host '  cmake --build <build> --target coverage'
    exit 1
}

$merged = Join-Path $BuildDir 'aurora.profdata'
& $profdata merge -sparse -o $merged @($raws | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: llvm-profdata merge failed.'; exit 1 }

# Coverage mapping is stored in each instrumented executable: the main binary + the rest appended via -object (llvm-cov multi-object aggregation).
$exes = Get-ChildItem -Path $BuildDir -Filter *.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(test_|bench_|demo_|aurora_|au-|gen_|ai_)' }
if (-not $exes -or $exes.Count -eq 0) {
    Write-Host 'ERROR: no instrumented executables found in build dir.'
    exit 1
}
$objArgs = @()
foreach ($e in $exes | Select-Object -Skip 1) { $objArgs += @('-object', $e.FullName) }

Write-Host '=== llvm-cov line coverage (src/ + include/aurora) ==='
& $llvmcov report ($exes[0].FullName) @objArgs `
    ("-instr-profile=" + $merged) `
    ("--ignore-filename-regex=third_party|_deps|tests[\\/]|tools[\\/]|examples[\\/]")
if ($LASTEXITCODE -ne 0) { Write-Host 'ERROR: llvm-cov report failed.'; exit 1 }

# ---- HTML report (enhanced, optional): llvm-cov show -format=html self-drawn interactive report ----
$htmlDir = Join-Path $BuildDir 'coverage-html'
Write-Host ('=== llvm-cov HTML report -> ' + $htmlDir + ' ===')
& $llvmcov show ($exes[0].FullName) @objArgs `
    ("-instr-profile=" + $merged) `
    ("--ignore-filename-regex=third_party|_deps|tests[\\/]|tools[\\/]|examples[\\/]") `
    "-format=html" `
    ("-output-dir=" + $htmlDir)
if ($LASTEXITCODE -ne 0) { Write-Host 'WARNING: llvm-cov show (HTML) failed; only terminal report available.' }


