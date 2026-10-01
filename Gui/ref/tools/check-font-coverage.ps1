# 校验 TTF 字形覆盖：解析 cmap，逐个检查码点是否存在。
# 用法: powershell -NoProfile -File check-coverage.ps1 <font.ttf> <source-dir>
param([string]$Font, [string]$SourceDir)

$ErrorActionPreference = 'Stop'

# ---- 读取字体字节 ----------------------------------------------------------
$b = [System.IO.File]::ReadAllBytes($Font)

# TTF 全部字段为**大端序**；BitConverter 在本机是小端，故手写读取。
function U16([int]$o) { return ([int]$b[$o] -shl 8) -bor [int]$b[$o + 1] }
function U32([int]$o) {
    return ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor
           ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3]
}

$numTables = U16 4
$cmapOff = -1
for ($i = 0; $i -lt $numTables; $i++) {
    $rec = 12 + $i * 16
    $tag = [System.Text.Encoding]::ASCII.GetString($b, $rec, 4)
    if ($tag -eq 'cmap') { $cmapOff = [int](U32 ($rec + 8)); break }
}
if ($cmapOff -lt 0) { throw "未找到 cmap 表" }

# ---- 选一个子表（优先 format 4 / 12）--------------------------------------
$nSub = U16 ($cmapOff + 2)
$best = -1; $bestFmt = 0
for ($i = 0; $i -lt $nSub; $i++) {
    $eo = $cmapOff + 4 + $i * 8
    $sub = $cmapOff + [int](U32 ($eo + 4))
    $fmt = U16 $sub
    if ($fmt -eq 4 -and $bestFmt -lt 4)   { $best = $sub; $bestFmt = 4 }
    if ($fmt -eq 12 -and $bestFmt -lt 12) { $best = $sub; $bestFmt = 12 }
}
if ($best -lt 0) { throw "未找到可用的 cmap 子表" }
Write-Host "cmap 子表格式: $bestFmt"

# ---- 建立已覆盖码点集合 ----------------------------------------------------
$covered = New-Object 'System.Collections.Generic.HashSet[int]'

if ($bestFmt -eq 4) {
    $segX2 = U16 ($best + 6)
    $seg = $segX2 / 2
    $endBase   = $best + 14
    $startBase = $endBase + $segX2 + 2
    for ($s = 0; $s -lt $seg; $s++) {
        $end   = U16 ($endBase + $s * 2)
        $start = U16 ($startBase + $s * 2)
        # 哨兵段（start=0xFFFF）表示无映射，跳过。
        if ($start -eq 0xFFFF) { continue }
        # 覆盖判定只看区间：idDelta / idRangeOffset 决定的是**字形 ID**，
        # 不决定码点是否被覆盖。早先误把 (c+delta) 当码点加入，导致覆盖率全错。
        for ($c = $start; $c -le $end; $c++) { [void]$covered.Add($c) }
    }
} else {
    $nGroups = U32 ($best + 12)
    for ($g = 0; $g -lt $nGroups; $g++) {
        $go = $best + 16 + $g * 12
        $sc = [int](U32 $go); $ec = [int](U32 ($go + 4))
        for ($c = $sc; $c -le $ec; $c++) { [void]$covered.Add($c) }
    }
}
Write-Host "字体覆盖码点数: $($covered.Count)"

# ---- 收集源码中的汉字 ------------------------------------------------------
$chars = New-Object 'System.Collections.Generic.HashSet[int]'
Get-ChildItem $SourceDir -Recurse -File -Filter *.dart | ForEach-Object {
    $t = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
    foreach ($ch in $t.ToCharArray()) {
        $cp = [int][char]$ch
        if ($cp -ge 0x4E00 -and $cp -le 0x9FFF) { [void]$chars.Add($cp) }
    }
}
Write-Host "源码唯一汉字数: $($chars.Count)"

# ---- 差集 ------------------------------------------------------------------
$missing = @()
foreach ($cp in $chars) { if (-not $covered.Contains($cp)) { $missing += [char]$cp } }
if ($missing.Count -eq 0) {
    Write-Host "`n结论: 全部汉字均已覆盖 —— 可以打包" -ForegroundColor Green
} else {
    Write-Host "`n结论: 缺 $($missing.Count) 个字形 —— 不可用" -ForegroundColor Red
    Write-Host ("缺失: " + ($missing -join ''))
    exit 1
}
