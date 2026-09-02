# coverage_report.ps1 - Coverage terminal summary for Aurora (no HTML).
# Invoked by the CMake `coverage` target. Prereq: tests built with --coverage and run.
#
# Two complementary metrics are printed:
#   (A) GCC gcov line coverage   - best-effort; MinGW/gcov needs -s <SrcRoot> because the
#       .gcno records ABSOLUTE Windows source paths that MinGW gcov cannot open otherwise.
#   (B) Public-header -> test mapping - reliable proxy: how many of the public headers under
#       include/aurora have a corresponding tests/test_*.cpp (the project's 1:1 test convention).
param(
    [string]$BuildDir = $PWD,
    [string]$SrcRoot  = (Split-Path -Parent $BuildDir)
)

$ErrorActionPreference = 'Continue'
$SrcRootFwd = $SrcRoot -replace '\\', '/'

# ---------------------------------------------------------------------------
# (A) gcov line coverage
# ---------------------------------------------------------------------------
Set-Location $SrcRoot

$gcdas = Get-ChildItem -Recurse -Path $BuildDir -Filter *.gcda -ErrorAction SilentlyContinue
if ($gcdas.Count -gt 0) {
    foreach ($g in $gcdas) {
        $objFwd  = ($g.DirectoryName -replace '\\', '/')
        $gcdaFwd = ($g.FullName -replace '\\', '/')
        & gcov -b -r -s $SrcRootFwd -o $objFwd $gcdaFwd > $null 2>&1
    }

    $rows = @()
    foreach ($f in (Get-ChildItem -Path $SrcRoot -Filter *.gcov -ErrorAction SilentlyContinue)) {
        $exec = 0; $miss = 0
        foreach ($line in (Get-Content $f.FullName)) {
            if ($line -match '^\s*\d+:') { $exec++ }
            elseif ($line -match '^\s*#####') { $miss++ }
        }
        $total = $exec + $miss
        if ($total -eq 0) { $pct = 100.0 } else { $pct = [math]::Round(100.0 * $exec / $total, 1) }
        $rows += New-Object PSObject -Property @{File = ($f.BaseName -replace '\.gcov$', ''); Coverage = $pct; Executed = $exec; Missed = $miss }
    }
    Get-ChildItem -Path $SrcRoot -Filter *.gcov -ErrorAction SilentlyContinue | Remove-Item -Force

    if ($rows.Count -gt 0) {
        $rows = $rows | Sort-Object Coverage
        Write-Host '=== gcov line coverage (best-effort) ==='
        $rows | Format-Table -AutoSize -Property File, Coverage, Executed, Missed
        $totE = ($rows | Measure-Object -Property Executed -Sum).Sum
        $totM = ($rows | Measure-Object -Property Missed   -Sum).Sum
        if (($totE + $totM) -eq 0) { $overall = 100.0 } else { $overall = [math]::Round(100.0 * $totE / ($totE + $totM), 1) }
        Write-Host ('OVERALL LINE COVERAGE: ' + $overall + '%')
        Write-Host 'NOTE: if this reads 0% everywhere, libgcov counters are not being recorded in this'
        Write-Host '      MinGW/GCC static-lib environment; rely on metric (B) below for actionable coverage.'
    }
}

# ---------------------------------------------------------------------------
# (A') HTML report (optional enhancement): use gcov_aggregate.py to draw HTML + CSV (shared shape
#      with the GCC branch and coverage_report.sh / LLVM branch). Depends on python3 +
#      gcov_aggregate.py in the same directory; silently skipped if missing, does not block local diagnosis.
# ---------------------------------------------------------------------------
$py = $null
foreach ($c in 'python3', 'python') { if (Get-Command $c -ErrorAction SilentlyContinue) { $py = $c; break } }
if ($py -and $gcdas.Count -gt 0) {
    $agg = Join-Path $SrcRoot 'tools/coverage/gcov_aggregate.py'
    if (Test-Path $agg) {
        $inter = Join-Path $env:TEMP "aurora_all.gcov"
        Remove-Item $inter -Force -ErrorAction SilentlyContinue
        foreach ($g in $gcdas) {
            $objFwd  = ($g.DirectoryName -replace '\\', '/')
            $gcdaFwd = ($g.FullName -replace '\\', '/')
            & gcov -i -t -o $objFwd $gcdaFwd 2>$null | Add-Content $inter
        }
        & $py $agg $inter --src-root $SrcRoot `
            --html (Join-Path $BuildDir 'coverage.html') `
            --csv  (Join-Path $BuildDir 'coverage.csv')
        Write-Host ('HTML report: ' + (Join-Path $BuildDir 'coverage.html'))
    }
}

# ---------------------------------------------------------------------------
# (B) Public-header -> test mapping (reliable proxy for test coverage)
# ---------------------------------------------------------------------------
$incRoot = Join-Path $SrcRoot 'include'
$testDir = Join-Path $SrcRoot 'tests'
$testFiles = @{}
if (Test-Path $testDir) {
    foreach ($t in (Get-ChildItem -Path $testDir -Filter test_*.cpp -ErrorAction SilentlyContinue)) {
        $testFiles[$t.BaseName] = $true
    }
}
$total = 0; $covered = 0; $uncovered = @()
if (Test-Path $incRoot) {
    foreach ($h in (Get-ChildItem -Recurse -Path $incRoot -Filter *.h -ErrorAction SilentlyContinue)) {
        $total++
        $base = $h.BaseName
        if ($testFiles['test_' + $base]) { $covered++ } else { $uncovered += ($h.FullName.Replace($SrcRoot, '').TrimStart('\', '/')) }
    }
}
$pct = if ($total -eq 0) { 100.0 } else { [math]::Round(100.0 * $covered / $total, 1) }
Write-Host ''
Write-Host '=== public header -> test_*.cpp mapping ==='
Write-Host ('COVERED: ' + $covered + '/' + $total + ' (' + $pct + '%)')
if ($uncovered.Count -gt 0) {
    Write-Host 'Headers WITHOUT a test_*.cpp:'
    $uncovered | Sort-Object | ForEach-Object { Write-Host ('  ' + $_) }
}


