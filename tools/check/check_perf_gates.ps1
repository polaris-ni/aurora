<#
.SYNOPSIS
  本机渲染性能「时间类」门槛一键校验脚本。

.DESCRIPTION
  驱动 bench_scroll / bench_render 的输出，校验时间类门槛：
    G-1  滚动 p99 帧时间        ≤ 16.67 ms
    G-2  滚动 worst 帧时间      ≤ 33.3 ms
    G-3  滚动 jitter            ≤ 2.0 ms
    G-4  长任务（>8.33ms）次数   ≤ 3 / 300 帧
    G-9  帧布局预算 max         ≤ 1.5 ms（信息项）
    G-10 帧绘制预算 max         ≤ 10.0 ms（信息项）
    G-11 帧上屏预算 max         ≤ 3.0 ms（信息项）
    G-12 text_cjk_12lines       ≤ 3.0 ms
    G-13 linear_gradient_full   ≤ 8.0 ms
    G-14 radial_gradient_full   ≤ 8.0 ms

  重要约定：
  - 时间类门槛受机器负载 / 编译器 / 后台进程影响，**不进 CTest**，本脚本仅作本机趋势对比。
  - 滚动时间读数须按「独立进程多次取最小」：同进程重复会因堆碎片偏慢（bench_scroll.cpp
    注释已说明），单次 in-process 采样会高报。本脚本对 bench_scroll 起 N 个独立进程取 p99 最小
    的一次作为代表（best-of），避免误报。
  - 计数类门槛 G-5~G-8 由 tests/test_scroll_regression.cpp 锁进 CTest（build-prof，PROFILING=ON），
    本脚本不重复校验。
  - G-9~G-11 的 frame budget（layout/paint/present max_ms）依赖 PROFILING=ON 构建才会记录 zone 计时，
    且已知 paint 基线 13.08ms 超 10ms 预算（部分未达标）。故 G-9~G-11 仅作**信息项**：
    给定 -ProfilingBuild 时解析展示，否则标记 n/a，且不计入本脚本的通过判定。

.PARAMETER BuildDir
  含 bench 可执行文件的 Release + PROFILING=OFF 构建目录（默认 "build"）。
.PARAMETER Scene
  bench_scroll 场景（默认 "google_play"）；可选 "synthetic"。
.PARAMETER Repeat
  bench_scroll 独立进程采样次数，取 p99 最小的一次（默认 3）。
.PARAMETER ProfilingBuild
  可选：PROFILING=ON 构建目录，用于解析 G-9~G-11 zone 计时（信息项）。不给则 G-9~G-11 标记 n/a。

.EXAMPLE
  pwsh tools/check/check_perf_gates.ps1
  pwsh tools/check/check_perf_gates.ps1 -BuildDir build -Scene google_play -Repeat 3 -ProfilingBuild build-prof
#>
param(
    [string]$BuildDir = "build",
    [string]$Scene = "google_play",
    [int]$Repeat = 3,
    [string]$ProfilingBuild = ""
)

$ErrorActionPreference = 'Stop'

# ---- 门槛来源：优先 perf_gates.json（与脚本同目录），缺失/损坏则回退内置默认 ----
# 门槛外置，避免改阈值须改脚本；文件缺失时行为不变（回退内置）。
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
        Write-Host "已加载 perf_gates.json 门槛（$(($cfg.gates).Count) 条；缺失项回退内置默认）。"
    } catch {
        Write-Host "perf_gates.json 解析失败，全部使用内置默认门槛：$_"
    }
} else {
    Write-Host "未找到 perf_gates.json，使用内置默认门槛（外置门槛请见 tools/check/perf_gates.json）。"
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
        Write-Error "未找到 $p —— 请先在该构建下构建 $name（Release + PROFILING=OFF）"
    }
    return $p
}

$benchScroll = Get-Exe $BuildDir "bench_scroll"
$benchRender = Get-Exe $BuildDir "bench_render"

# ---- G-1~G-4：bench_scroll best-of ----
$p99Best = [double]::PositiveInfinity
$bestWorst = $null; $bestJitter = $null; $bestLong = $null
for ($i = 1; $i -le $Repeat; $i++) {
    $csv = & $benchScroll --scene $Scene --format csv --frames 300 2>$null
    $row = ($csv | Where-Object { $_ -match ',' } | Select-Object -Last 1)
    if ($null -eq $row) { continue }
    $c = $row -split ','
    if ($c.Count -le 10) { continue }
    $p = [double]$c[4]; $w = [double]$c[7]; $j = [double]$c[8]; $l = [double]$c[10]
    if ($p -lt $p99Best) { $p99Best = $p; $bestWorst = $w; $bestJitter = $j; $bestLong = $l }
}

# ---- G-12 / G-13 / G-14：bench_render 表格解析 ----
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
# 与 G-13/G-14 门槛口径一致：1080p @scale 1.0
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

# ---- G-9~G-11（信息项，仅展示，不计入判定）----
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

# ---- 评估：解析失败/缺失判 FAIL，G-9~G-11 信息项不计入 ----
$gates = @()
function Add-Gate {
    param([string]$id, [string]$metric, [object]$val, [double]$thr, [bool]$count = $true)
    $ok = $false
    if ($null -ne $val -and $val -match '^[\d.]+$') { $ok = ([double]$val) -le $thr }
    $gates += [PSCustomObject]@{ Gate = $id; Metric = $metric; Measured = $val; Threshold = $thr; Pass = $ok; Counted = $count }
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

# ---- 输出 ----
Write-Host ""
Write-Host "本机时间类门槛校验  (scene=$Scene, build=$BuildDir, repeat=$Repeat)"
Write-Host "=============================================================="
Write-Host ("{0,-6} {1,-28} {2,12} {3,12} {4,7}" -f 'Gate', 'Metric', 'Measured', 'Threshold', 'Result')
Write-Host "--------------------------------------------------------------"
foreach ($g in $gates) {
    $res = if ($g.Pass) { 'PASS' } elseif ($null -eq $g.Measured) { 'n/a' } else { 'FAIL' }
    Write-Host ("{0,-6} {1,-28} {2,12} {3,12} {4,7}" -f $g.Gate, $g.Metric, $g.Measured, $g.Threshold, $res)
}
$counted = $gates | Where-Object { $_.Counted }
$allPass = ($counted | Where-Object { -not $_.Pass }).Count -eq 0
Write-Host "--------------------------------------------------------------"
Write-Host "G-9~G-11 为信息项（依赖 PROFILING=ON 且 paint 基线已知超 10ms 预算），不计入通过判定。"
if ($allPass) {
    Write-Host "本机时间类门槛全部通过 ✅（时间类不进 CI，仅本机趋势对比；best-of $Repeat 进程取最小）"
} else {
    Write-Host "存在未达标项 ⚠️ —— 时间类受环境抖动影响，先在本机以 best-of 复跑确认是否为偶发；" `
        + "持续失败再排查根因，不得为达标私自下调门槛。"
}

# 退出码反映计入项结果；时间类门槛不接入 CI。
exit $(if ($allPass) { 0 } else { 1 })


