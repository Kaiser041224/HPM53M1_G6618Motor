# ============================================================================
# extract-device-art.ps1 —— 从固件终端源码提取设备 ASCII 艺术字
# ----------------------------------------------------------------------------
# 上位机的设备标识必须与固件 USB 终端显示的字形、色序完全一致，
# 否则"同一个设备"在两处会长得不一样。本脚本从固件源码提取，杜绝手抄出错。
#
# 用法：  powershell -NoProfile -File tools\extract-device-art.ps1
# 输出：  字形文本 + 每行的 256 色号 + 对应 RGB，可直接粘进模板
#
# 注意：本文件必须保留 UTF-8 BOM（Windows PowerShell 5.1 会把无 BOM 的
#       UTF-8 脚本按 ANSI 码页解码，导致中文注释与块字符损坏）。
# ============================================================================

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) 'App\Comm\terminal\Src\app_terminal.c'
if (-not (Test-Path $src)) { throw "找不到固件终端源码: $src" }

# 关键：必须显式按 UTF-8 读取，否则 █ 与 │ 会被解码成乱码
$lines = [System.IO.File]::ReadAllLines($src, [System.Text.Encoding]::UTF8)

# 定位 banner 数组，再找其中「仅由块字符与空格构成」的连续行 = 设备艺术字
$startIdx = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 's_banner_lines\[\]') { $startIdx = $i; break }
}
if ($startIdx -lt 0) { throw "未找到 s_banner_lines 定义" }

$block = [char]0x2588   # █
$bar   = [char]0x2502   # │

$art = @()
$colors = @()
$i = $startIdx
while ($i -lt $lines.Count -and $i -lt $startIdx + 60) {
    $raw = $lines[$i]
    if ($raw -match '^\s*\};') { break }

    # 取两个 │ 之间的内容
    $a = $raw.IndexOf($bar)
    $b = $raw.LastIndexOf($bar)
    if ($a -ge 0 -and $b -gt $a) {
        $inner = $raw.Substring($a + 1, $b - $a - 1)
        # 只在「含块字符且不含其它可见文案」时视为艺术字行
        $hasBlock = $inner.IndexOf($block) -ge 0
        $stripped = $inner.Replace([string]$block, '').Trim()
        if ($hasBlock -and $stripped.Length -eq 0) {
            $art += $inner
            $m = [regex]::Match($raw, '\\033\[38;5;(\d+)m')
            $colors += if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
        }
    }
    $i++
}

if ($art.Count -eq 0) { throw "未提取到艺术字（源码结构可能已变更）" }

# 去公共左右空白，得到紧凑字形
$padL = ($art | ForEach-Object { $_.Length - $_.TrimStart().Length } | Measure-Object -Minimum).Minimum
$padR = ($art | ForEach-Object { $_.TrimEnd().Length }               | Measure-Object -Maximum).Maximum
$compact = $art | ForEach-Object { $_.Substring($padL, $padR - $padL) }

# 256 色 → RGB（仅覆盖 16..231 的 6×6×6 立方，够本项目用）
function Convert-256ToRgb([int]$n) {
    if ($n -lt 16) { return $null }
    if ($n -lt 232) {
        $n -= 16
        $r = [int][Math]::Floor($n / 36)
        $g = [int][Math]::Floor(($n % 36) / 6)
        $b = $n % 6
        $lv = @(0, 95, 135, 175, 215, 255)
        return @($lv[$r], $lv[$g], $lv[$b])
    }
    return $null
}

Write-Host "设备 ASCII 艺术字（提取自 $([System.IO.Path]::GetFileName($src))）"
Write-Host ("字形尺寸: {0} 列 x {1} 行`n" -f ($padR - $padL), $compact.Count)
Write-Host "=== 字形 ==="
for ($k = 0; $k -lt $compact.Count; $k++) { Write-Host $compact[$k] }

Write-Host "`n=== 色序（终端 256 色 → RGB）==="
for ($k = 0; $k -lt $colors.Count; $k++) {
    $rgb = Convert-256ToRgb $colors[$k]
    $hex = if ($rgb) { '#{0:X2}{1:X2}{2:X2}' -f $rgb[0], $rgb[1], $rgb[2] } else { '—' }
    Write-Host ("  第 {0} 行  256色 {1,3}  →  {2}" -f ($k + 1), $colors[$k], $hex)
}

Write-Host "`n=== 可直接粘进 HTML 的片段 ==="
for ($k = 0; $k -lt $compact.Count; $k++) {
    Write-Host ("<span class=`"r{0}`">{1}</span>" -f ($k + 1), $compact[$k])
}
Write-Host "`n提示：CSS 中 .dev-art .rN 的 color 请填上表 RGB，text-shadow 用同色多层叠加。"
