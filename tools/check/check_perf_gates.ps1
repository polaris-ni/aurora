<#
.SYNOPSIS
  One-click local check for render-performance "time-class" gates.

.DESCRIPTION
  Drives the output of bench_scroll / bench_render and validates the time-class gates:
    G-1  scroll p99 frame time         <= 16.67 ms
    G-2  scroll worst frame time       <= 33.3 ms
    G-3  scroll jitter                 <= 2.0 ms
    G-4  long tasks (>8.33 ms) count    <= 3 / 300 frames
    G-9  frame layout budget max        <= 1.5 ms (info only)
    G-10 frame paint budget max         <= 10.0 ms (info only)
    G-11 frame present budget max       <= 3.0 ms (info only)
    G-12 text_cjk_12lines              <= 3.0 ms
    G-13 linear_gradient_full          <= 8.0 ms
    G-14 radial_gradient_full          <= 8.0 ms

  Important conventions:
  - Time-class gates are affected by machine load / compiler / background processes, so they are
    **NOT in CTest**; this script is for local trend comparison only.
  - Scroll timings must follow "take the minimum across multiple independent processes": in-process
    repetition reads slower due to heap fragmentation (explained in bench_scroll.cpp), and a single
    in-process sample over-reports. This script launches N independent processes for bench_scroll and
    takes the lowest p99 as the representative (best-of) to avoid false failures.
  - Counter-class gates G-5~G-8 are locked into CTest by tests/unit/utest_scroll.cpp (case
    scroll_regression_counter_gates), which reads their thresholds from this same perf_gates.json by gate id
    (needs PROFILING=ON, else it registers a skip stub). This script parses only the time-class gates and does
    not re-check them.
  - G-9~G-11 frame budgets (layout/paint/present max_ms) are only recorded with a PROFILING=ON build,
    and the known paint baseline 13.08ms exceeds the 10ms budget (some are not met). Hence G-9~G-11
    are **info-only**: they are parsed and shown when -ProfilingBuild is given, otherwise marked n/a
    and excluded from this script's pass/fail decision.

.PARAMETER BuildDir
  Release + PROFILING=OFF build dir containing the bench executables (default "build").
.PARAMETER Scene
  bench_scroll scene (default "app"); "synthetic" / "lazy" / "grid" also available.
.PARAMETER Repeat
  Number of independent bench_scroll process samples; takes the lowest p99 (default 3).
.PARAMETER ProfilingBuild
  Optional: PROFILING=ON build dir used to parse G-9~G-11 zone timings (info only). If not given,
  G-9~G-11 are marked n/a.

.EXAMPLE
  pwsh tools/check/check_perf_gates.ps1
  pwsh tools/check/check_perf_gates.ps1 -BuildDir build -Scene app -Repeat 3 -ProfilingBuild build-prof
#>
param(
    [string]$BuildDir = "build",
    [string]$Scene = "app",
    [int]$Repeat = 3,
    [string]$ProfilingBuild = ""
)

$ErrorActionPreference = 'Stop'

# ---- Gate source: prefer perf_gates.json (same dir as this script); fall back to built-in defaults
# if missing/corrupt ---- Externalized thresholds so changing them does not require editing the script;
# behavior is unchanged when the file is absent (falls back to built-in).
$BuiltinThr = @{
    'G-1|scroll p99 (ms, best-of)'    = 16.67
    'G-2|scroll worst (ms)'           = 33.3
    'G-3|scroll jitter (ms)'          = 2.0
    'G-4|long tasks / 300'            = 3
    'G-9|frame layout max (ms)'       = 1.5
    'G-10|frame paint max (ms)'       = 10.0
    'G-11|frame present max (ms)'     = 3.0
    'G-12|text_cjk_12lines @1080p'    = 3.0
    'G-13|linear_gradient_full @1080p'  = 8.0
    'G-14|radial_gradient_full @1080p' = 8.0
}
$GateThr = @{}
$gateJson = Join-Path $PSScriptRoot 'perf_gates.json'
if (Test-Path $gateJson) {
    try {
        $cfg = Get-Content $gateJson -Raw | ConvertFrom-Json
        foreach ($g in $cfg.gates) {
            $GateThr["$($g.id)|$($g.metric)"] = [double]$g.threshold
        }
        Write-Host "Loaded perf_gates.json gates ($(($cfg.gates).Count) entries; missing entries fall back to built-in defaults)."
    } catch {
        Write-Host "perf_gates.json parse failed; using all built-in default thresholds: $_"
    }
} else {
    Write-Host "perf_gates.json not found; using built-in defaults (externalized gates: tools/check/perf_gates.json)."
}
function Get-Thr {
    param([string]$id, [string]$metric)
    $k = "$id|$metric"
    if ($GateThr.ContainsKey($k)) { return $GateThr[$k] }
    return $BuiltinThr[$k]
}

function Get-Exe {
    param([string]$dir, [string]$name)
    $p = Join-Path $dir "$name.exe"
    if (-not (Test-Path $p)) {
        Write-Error "Not found: $p -- build $name in that build first (Release + PROFILING=OFF)"
    }
    return $p
}

$benchScroll = Get-Exe $BuildDir "bench_scroll"
$benchRender = Get-Exe $BuildDir "bench_render"

# ---- G-1~G-4：bench_scroll best-of ----
# 列位按 CSV 表头名取（表头定义见 src/aurora/perf/perf_session.cpp 的 summary_line），
# 不再按硬编码下标猜：早期版本写死 $c[4]，而该列实为 p95_ms，把 p99 门槛放松了一个百分位。
$p99Best = [double]::PositiveInfinity
$bestWorst = $null; $bestJitter = $null; $bestLong = $null
for ($i = 1; $i -le $Repeat; $i++) {
    $csv = & $benchScroll --scene $Scene --format csv --frames 300 2>$null
    $lines = @($csv | Where-Object { $_ -match ',' })
    if ($lines.Count -lt 2) { continue }
    $cols = @($lines[0] -split ',')
    $cells = @($lines[-1] -split ',')
    if ($cells.Count -ne $cols.Count) { continue }
    $ix = @{}
    for ($k = 0; $k -lt $cols.Count; $k++) { $ix[$cols[$k].Trim()] = $k }
    $need = @('p99_ms', 'worst_ms', 'jitter_ms', 'long_task_count')
    $missing = @($need | Where-Object { -not $ix.ContainsKey($_) })
    if ($missing.Count -gt 0) { continue }
    $p = [double]$cells[$ix['p99_ms']]; $w = [double]$cells[$ix['worst_ms']]
    $j = [double]$cells[$ix['jitter_ms']]; $l = [double]$cells[$ix['long_task_count']]
    if ($p -lt $p99Best) { $p99Best = $p; $bestWorst = $w; $bestJitter = $j; $bestLong = $l }
}

# ---- G-12 / G-13 / G-14: bench_render table parsing ----
$renderMd = & $benchRender 2>$null
function Parse-RowMS {
    param([string[]]$lines, [string]$scene)
    foreach ($l in $lines) {
        $cells = $l -split '\|' | ForEach-Object { $_.Trim() }
        if ($cells.Count -ge 6 -and $cells[1] -eq $scene) {
            $v = $cells[5] -replace ' ms', ''
            if ($v -match '^[\d.]+$') { return $v }
        }
    }
    return $null
}
# Consistent with the G-13/G-14 gate convention: 1080p @scale 1.0
function Parse-RowMSAt {
    param([string[]]$lines, [string]$scene, [string]$size, [string]$scale)
    foreach ($l in $lines) {
        $cells = $l -split '\|' | ForEach-Object { $_.Trim() }
        if ($cells.Count -ge 6 -and $cells[1] -eq $scene -and $cells[2] -eq $size -and $cells[3] -eq $scale) {
            $v = $cells[5] -replace ' ms', ''
            if ($v -match '^[\d.]+$') { return $v }
        }
    }
    return $null
}
$linG   = Parse-RowMSAt $renderMd 'linear_gradient_full'  '1920x1080' '1.0'
$radG   = Parse-RowMSAt $renderMd 'radial_gradient_full' '1920x1080' '1.0'
$textCjk = Parse-RowMSAt $renderMd 'text_cjk_12lines'     '1920x1080' '1.0'

# ---- G-9~G-11 (info only, shown but not counted in the decision) ----
$paintMax = $null; $layoutMax = $null; $presentMax = $null
if ($ProfilingBuild -ne "") {
    $pbScroll = Get-Exe $ProfilingBuild "bench_scroll"
    $md = & $pbScroll --scene $Scene --format md --frames 120 2>$null
    foreach ($l in $md) {
        $cells = $l -split '\|' | ForEach-Object { $_.Trim() }
        if ($cells.Count -ge 5) {
            if ($cells[1] -eq 'Window::paint')   { $paintMax   = $cells[4] }
            if ($cells[1] -eq 'Window::layout')  { $layoutMax  = $cells[4] }
            if ($cells[1] -eq 'Window::present') { $presentMax = $cells[4] }
        }
    }
}

# ---- Evaluation: parse failure/missing => FAIL; G-9~G-11 info items are not counted ----
$gates = @()
function Add-Gate {
    param([string]$id, [string]$metric, [object]$val, [double]$thr, [bool]$count = $true)
    # 无读数（null / ∞ / 非数字）一律记为未达标：否则基准程序改名、参数变化或输出形态一改，
    # 整张表就会「空表全绿」——这正是本脚本早期长期返回 exit 0 的机理（函数体内 `$gates +=`
    # 在 PowerShell 里创建的是局部变量，外层数组始终为空，判定 `(-not Pass).Count -eq 0` 恒真）。
    $shown = 'n/a'
    $ok = $false
    if ($null -ne $val -and "$val" -match '^[\d.]+$') { $shown = "$val"; $ok = ([double]$val) -le $thr }
    $script:gates += [PSCustomObject]@{ Gate = $id; Metric = $metric; Measured = $shown; Threshold = $thr; Pass = $ok; Counted = $count }
}
Add-Gate 'G-1'  'scroll p99 (ms, best-of)' $p99Best    (Get-Thr 'G-1' 'scroll p99 (ms, best-of)')
Add-Gate 'G-2'  'scroll worst (ms)'         $bestWorst  (Get-Thr 'G-2' 'scroll worst (ms)')
Add-Gate 'G-3'  'scroll jitter (ms)'        $bestJitter (Get-Thr 'G-3' 'scroll jitter (ms)')
Add-Gate 'G-4'  'long tasks / 300'          $bestLong   (Get-Thr 'G-4' 'long tasks / 300')
Add-Gate 'G-9'  'frame layout max (ms)'     $layoutMax  (Get-Thr 'G-9' 'frame layout max (ms)')  $false
Add-Gate 'G-10' 'frame paint max (ms)'      $paintMax   (Get-Thr 'G-10' 'frame paint max (ms)')  $false
Add-Gate 'G-11' 'frame present max (ms)'    $presentMax (Get-Thr 'G-11' 'frame present max (ms)') $false
Add-Gate 'G-12' 'text_cjk_12lines @1080p'   $textCjk   (Get-Thr 'G-12' 'text_cjk_12lines @1080p')
Add-Gate 'G-13' 'linear_gradient_full @1080p' $linG     (Get-Thr 'G-13' 'linear_gradient_full @1080p')
Add-Gate 'G-14' 'radial_gradient_full @1080p' $radG     (Get-Thr 'G-14' 'radial_gradient_full @1080p')

# ---- Output ----
Write-Host ""
Write-Host "Local time-class gate check  (scene=$Scene, build=$BuildDir, repeat=$Repeat)"
Write-Host "=============================================================="
Write-Host ("{0,-6} {1,-28} {2,12} {3,12} {4,7}" -f 'Gate', 'Metric', 'Measured', 'Threshold', 'Result')
Write-Host "--------------------------------------------------------------"
foreach ($g in $gates) {
    # 计入项无读数 = 失败（写 FAIL(no data) 逼人来查为什么没测到）；信息项无读数按脚本自述记 n/a。
    if ($g.Pass) {
        $res = 'PASS'
    } elseif ($g.Measured -eq 'n/a') {
        $res = if ($g.Counted) { 'FAIL(no data)' } else { 'n/a' }
    } else {
        $res = 'FAIL'
    }
    Write-Host ("{0,-6} {1,-28} {2,12} {3,12} {4,7}" -f $g.Gate, $g.Metric, $g.Measured, $g.Threshold, $res)
}
$counted = @($gates | Where-Object { $_.Counted })
# 表里一条计入项都没有也判失败：空表不等于通过。
$allPass = ($counted.Count -gt 0) -and (@($counted | Where-Object { -not $_.Pass }).Count -eq 0)
Write-Host "--------------------------------------------------------------"
Write-Host "G-9~G-11 are info-only (depend on PROFILING=ON and a known paint baseline over the 10ms budget); excluded from the pass decision."
if ($allPass) {
    Write-Host "All local time-class gates passed ✅ (time-class gates are not in CI; local trend comparison only; best-of $Repeat processes takes the minimum)"
} else {
    Write-Host "Some gates not met ⚠️ -- time-class gates are affected by environment jitter; re-run locally with best-of first to confirm it is sporadic; " `
        + "investigate the root cause on persistent failure; do not lower the threshold just to pass."
}

# Exit code reflects the counted items; time-class gates are not wired into CI.
exit $(if ($allPass) { 0 } else { 1 })


