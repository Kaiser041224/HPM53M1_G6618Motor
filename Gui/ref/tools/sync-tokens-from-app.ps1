# ============================================================================
# sync-tokens-from-app.ps1 —— 以 App 实现为准，回写设计手册的色板
# ----------------------------------------------------------------------------
# 单一事实源是 **App 的 `lib/theme/gui_palette.dart`**（开发时看着实际屏幕调的）。
# 本脚本从中提取全部颜色常量，重写 `tokens.css` 里的色板段与
# `design-system.md` 的色板表，避免两份手册各自漂移。
#
# 用法：powershell -NoProfile -File tools\sync-tokens-from-app.ps1
# 注意：本文件必须保留 UTF-8 BOM。
# ============================================================================

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$dartPath = Join-Path (Split-Path -Parent $root) 'app\lib\theme\gui_palette.dart'
$cssPath  = Join-Path $root 'design-system\tokens.css'
$mdPath   = Join-Path $root 'design-system\design-system.md'

if (-not (Test-Path $dartPath)) { throw "找不到 App 调色板: $dartPath" }

# ---- 从 Dart 提取颜色常量 --------------------------------------------------
$src = [System.IO.File]::ReadAllText($dartPath, [System.Text.Encoding]::UTF8)

# `this.name = const Color(0xFFRRGGBB),`
$colors = [ordered]@{}
foreach ($m in [regex]::Matches($src, 'this\.([A-Za-z0-9_]+)\s*=\s*const Color\(0xFF([0-9A-Fa-f]{6})\)')) {
    $colors[$m.Groups[1].Value] = '#' + $m.Groups[2].Value.ToUpper()
}
# `this.name = const Color(0xAARRGGBB),`（AARRGGBB，AA ≠ FF）—— v5 groupLine 等带 alpha 的实色。
foreach ($m in [regex]::Matches($src, 'this\.([A-Za-z0-9_]+)\s*=\s*const Color\(0x([0-9A-Fa-f]{8})\)')) {
    if ($m.Groups[2].Value.Substring(0, 2) -ne 'FF') {
        $colors[$m.Groups[1].Value] = '#' + $m.Groups[2].Value.ToUpper()
    }
}
# `this.name = const Color.fromRGBO(r, g, b, a),`
$rgbo = [ordered]@{}
foreach ($m in [regex]::Matches($src, 'this\.([A-Za-z0-9_]+)\s*=\s*const Color\.fromRGBO\((\d+),\s*(\d+),\s*(\d+),\s*([0-9.]+)\)')) {
    $rgbo[$m.Groups[1].Value] = @([int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value, [double]$m.Groups[5].Value)
}

if ($colors.Count -eq 0) { throw "未从 gui_palette.dart 提取到任何颜色" }
Write-Host "从 App 提取到 $($colors.Count) 个实色、$($rgbo.Count) 个透明色"

# ---- CSS 变量名映射（Dart camelCase -> CSS kebab-case）----------------------
function To-Kebab([string]$s) {
    return ([regex]::Replace($s, '([A-Z])', '-$1')).ToLower()
}

# ---- 重写 tokens.css 的色板段 ---------------------------------------------
$css = [System.IO.File]::ReadAllText($cssPath, [System.Text.Encoding]::UTF8)

# 语义分组：与 gui_palette.dart 的字段注释保持一致
$groups = [ordered]@{
    '基底（App 为准）'        = @('bgVoid','bgBase','bgPanel','bgRaise','bgHover','bgActive')
    '发丝边框与组线'          = @('lineHi','line','lineDim','groupLine')
    '文字'                    = @('fg','fgStrong','fgDim','fgMute')
    '强调'                    = @('accent','accent2','accentBlue')
    '状态语义'                = @('stNormal','stWarn','stFault','stLatched','stStale','stPending')
    '语义读数色（v5 量纲专色）' = @('valueAmber','valueCyan','valueGreen','valueOrange','valueRed','valueMagenta','valuePurple')
    '数据系列'                = @('series1','series2','series3','series4','series5','series6')
    '按钮'                    = @('btnFill','btnBorder','btnText','btnPrimaryBorder','btnPrimaryText','btnDangerBorder','btnDangerText')
    '齿轮示意'                = @('gearGreen','gearGreenRim','gearViolet','gearVioletRim')
    '辉光'                    = @('glowSoft','glow1','glow2','glow3','glow4','stroke')
}

$sb = New-Object System.Text.StringBuilder
foreach ($g in $groups.Keys) {
    [void]$sb.AppendLine("  /* ---------- $g ---------- */")
    foreach ($k in $groups[$g]) {
        if ($colors.Contains($k)) {
            # 名字带冒号后再补空格对齐 —— 冒号必须紧跟变量名，否则整行是无效 CSS。
            $cssName = '--' + (To-Kebab $k) + ':'
            [void]$sb.AppendLine(('  {0,-19} {1};' -f $cssName, $colors[$k]))
        } elseif ($rgbo.Contains($k)) {
            $v = $rgbo[$k]
            $cssName = '--' + (To-Kebab $k) + ':'
            [void]$sb.AppendLine(('  {0,-19} rgba({1}, {2}, {3}, {4});' -f $cssName, $v[0], $v[1], $v[2], $v[3]))
        }
    }
    [void]$sb.AppendLine('')
}
$block = $sb.ToString().TrimEnd()

# 只替换标记区间内的色板段 —— `:root` 里还有字体、排版、间距等
# 非颜色 token，整体替换会把它们一并删掉。
$begin = '/* >>> GENERATED-COLORS-BEGIN'
$end   = '/* <<< GENERATED-COLORS-END <<< */'
$bi = $css.IndexOf($begin)
$ei = $css.IndexOf($end)
if ($bi -lt 0 -or $ei -lt 0 -or $ei -le $bi) {
    throw "tokens.css 缺少生成标记（$begin / $end）—— 请确认标记未被删除"
}
# head 保留到 BEGIN 标记那一行**结束**（含换行），否则标记会被吃掉。
$beginLineEnd = $css.IndexOf("`n", $bi)
$headKeep = $css.Substring(0, $beginLineEnd + 1)
$tail = $css.Substring($ei)
$css = $headKeep + $block + "`r`n`r`n  " + $tail
[System.IO.File]::WriteAllText($cssPath, $css, [System.Text.UTF8Encoding]::new($false))
Write-Host "已回写 tokens.css（仅色板段）"

# ---- 打印对照表，便于人工核对 ---------------------------------------------
Write-Host "`n=== 色板（App 为准）==="
foreach ($g in $groups.Keys) {
    Write-Host "  [$g]"
    foreach ($k in $groups[$g]) {
        if ($colors.Contains($k)) { Write-Host ("    {0,-18} {1}" -f $k, $colors[$k]) }
    }
}
Write-Host "`ntokens.css 与 gui_palette.dart 现已一致。"
