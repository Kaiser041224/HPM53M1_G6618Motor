# ============================================================================
# measure-colors.ps1 —— 从 screenshots/ 复现设计系统的色板测量
# ----------------------------------------------------------------------------
# 这是 design-system.md 中所有色值的来源脚本。改动设计系统前先跑一遍，
# 确保结论仍然来自实测而非记忆。
#
# 用法：  pwsh -File tools/measure-colors.ps1
# 输出：  基底色占比 + 按色相家族的强调色峰值
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$shotDir = Join-Path $root 'screenshots'

if (-not (Test-Path $shotDir)) { throw "找不到参考图目录: $shotDir" }
$imgs = Get-ChildItem -Path "$shotDir\*" -File |
        Where-Object { $_.Extension -match '^\.(png|jpg|jpeg|webp)$' }
if (-not $imgs) { throw "$shotDir 下没有图片" }

Write-Host "参考图: $($imgs.Count) 张`n"

# ---- 色相计算 --------------------------------------------------------------
function Get-Hue([int]$r, [int]$g, [int]$b) {
    $max = [Math]::Max($r, [Math]::Max($g, $b))
    $min = [Math]::Min($r, [Math]::Min($g, $b))
    $d = $max - $min
    if ($d -eq 0) { return -1 }
    if ($max -eq $r)      { $h = 60 * ((($g - $b) / $d) % 6) }
    elseif ($max -eq $g)  { $h = 60 * ((($b - $r) / $d) + 2) }
    else                  { $h = 60 * ((($r - $g) / $d) + 4) }
    if ($h -lt 0) { $h += 360 }
    return $h
}

# 色相家族（度）—— 与 design-system.md 一致
$families = [ordered]@{
    '青   cyan    (160-200)' = @(160, 200)
    '绿   green   ( 90-160)' = @( 90, 160)
    '黄   yellow  ( 40- 70)' = @( 40,  70)
    '品红 magenta (280-345)' = @(280, 345)
    '红   red     (345- 15)' = @(345, 375)
    '蓝   blue    (200-250)' = @(200, 250)
}

$base = @{}          # 基底色（暗部）计数
$famPeak = @{}       # 各家族峰值色计数
$famCount = @{}
foreach ($k in $families.Keys) { $famPeak[$k] = @{}; $famCount[$k] = 0 }

foreach ($img in $imgs) {
    $bmp = [System.Drawing.Bitmap]::FromFile($img.FullName)
    Write-Host ("  扫描 {0}  ({1}x{2})" -f $img.Name, $bmp.Width, $bmp.Height)
    for ($y = 0; $y -lt $bmp.Height; $y += 2) {
        for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            $p = $bmp.GetPixel($x, $y)
            $r = [int]$p.R; $g = [int]$p.G; $bl = [int]$p.B
            $mx = [Math]::Max($r, [Math]::Max($g, $bl))
            $mn = [Math]::Min($r, [Math]::Min($g, $bl))
            if ($mx -eq 0) { continue }
            $sat = ($mx - $mn) / [double]$mx
            $lum = ($mx + $mn) / 2.0

            # 基底：暗且低饱和 → 量化到 8 阶
            if ($lum -lt 46 -and $sat -lt 0.75) {
                $key = '#{0:X2}{1:X2}{2:X2}' -f ([int]($r/8)*8), ([int]($g/8)*8), ([int]($bl/8)*8)
                if ($base.ContainsKey($key)) { $base[$key]++ } else { $base[$key] = 1 }
            }

            # 强调：高饱和 + 够亮 → 按色相家族归类
            if ($sat -ge 0.40 -and $lum -ge 80) {
                $h = Get-Hue $r $g $bl
                if ($h -lt 0) { continue }
                $h2 = if ($h -lt 15) { $h + 360 } else { $h }
                foreach ($k in $families.Keys) {
                    if ($h2 -ge $families[$k][0] -and $h2 -le $families[$k][1]) {
                        $famCount[$k]++
                        $key = '#{0:X2}{1:X2}{2:X2}' -f ([int]($r/8)*8), ([int]($g/8)*8), ([int]($bl/8)*8)
                        if ($famPeak[$k].ContainsKey($key)) { $famPeak[$k][$key]++ } else { $famPeak[$k][$key] = 1 }
                        break
                    }
                }
            }
        }
    }
    $bmp.Dispose()
}

$baseTotal = ($base.Values | Measure-Object -Sum).Sum
Write-Host "`n=== 基底色（暗部，占比） ==="
$base.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 8 | ForEach-Object {
    Write-Host ("  {0}  {1,8} px  {2,6:P1}" -f $_.Key, $_.Value, ($_.Value / $baseTotal))
}

Write-Host "`n=== 强调色（按色相家族，峰值色） ==="
foreach ($k in $families.Keys) {
    if ($famCount[$k] -eq 0) { Write-Host ("  {0}  —" -f $k); continue }
    $top = $famPeak[$k].GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3
    $s = ($top | ForEach-Object { "$($_.Key)(x$($_.Value))" }) -join '  '
    Write-Host ("  {0}  {1,7} px   {2}" -f $k, $famCount[$k], $s)
}

Write-Host "`n提示：若峰值色与 design-system/tokens.css 不符，说明参考图已更换，需同步更新 tokens。"
