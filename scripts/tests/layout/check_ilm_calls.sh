#!/usr/bin/env bash
# ILM 跨段调用守卫：`.fast`（ILM）段内的函数不得跳回 XIP flash 取指。
#
# 背景（2026-10-28）：
#   把函数放进 ILM 只为省掉取指延迟。若 ILM 内的函数又调用 XIP 中的函数，
#   每次调用都要回 flash 取指，ILM 的意义被部分抵消——这类「ILM 债」看不见、
#   不会报错，只能靠反汇编发现（本批一次就查出 26 处）。
#
# 判据：
#   `.fast` 段内所有直接 jal / jalr 的目标地址若 >= 0x8000_0000（XIP），
#   则目标符号必须在白名单内。白名单只允许三类：
#     1) 一次性初始化（启动期执行，不在周期路径）
#     2) SDK / ROM API（不可改，且调用点已有关中断或非实时语义）
#     3) libm（精度不可替换，且调用频率 ≤1kHz）
#
# 用法：scripts/tests/layout/check_ilm_calls.sh [elf]
# 环境：GNURISCV_TOOLCHAIN_PATH（默认 /opt/riscv32-gnu-toolchain-elf-bin）

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
ELF="${1:-$ROOT/output/HPM53M1_G6618Motor.elf}"

TOOLCHAIN="${GNURISCV_TOOLCHAIN_PATH:-/opt/riscv32-gnu-toolchain-elf-bin}"
OBJDUMP="$TOOLCHAIN/bin/riscv32-unknown-elf-objdump"

if [[ ! -x "$OBJDUMP" ]]; then
    echo "FAIL: objdump 不可执行: $OBJDUMP（设 GNURISCV_TOOLCHAIN_PATH）" >&2
    exit 2
fi
if [[ ! -f "$ELF" ]]; then
    echo "FAIL: ELF 不存在: $ELF（先 make artifacts）" >&2
    exit 2
fi

# 允许的 XIP 目标（新增条目必须在此写清理由）
ALLOW=(
    # —— 一次性初始化（不在周期路径）——
    clock_get_frequency              # intf_clock_get_cpu_freq：init/诊断期
    intf_hrpwm_init_pair             # PWM 通道配置
    hrpwm_apply_phase                # PWM 配置期写相位
    hrpwm_delay_ns_to_ticks          # PWM 配置期换算
    foc_math_init                    # sin/cos 表建表（含 sinf，禁止在 ISR 首调）
    app_hardware_params_init         # 参数单例懒初始化兜底
    app_software_params_init         # 参数单例懒初始化兜底
    # —— SDK / ROM / libc（不可改）——
    rom_xpi_nor_read                 # ROM XPI 读（调用期间已关中断）
    l1c_dc_invalidate                # SDK cache 操作
    memset                           # fault_reset_all 清结构体（clear 路径，低频）
    # —— libm（精度不可替换，频率 ≤1kHz）——
    sqrtf                            # RMS 真有效值
    # —— 显式例外 ——
    app_foc_disable                  # 含 20ms 软关断延时，必须任务上下文（ISR 禁止）
)

TMP_ASM="$(mktemp)"
TMP_RES="$(mktemp)"
trap 'rm -f "$TMP_ASM" "$TMP_RES"' EXIT

"$OBJDUMP" -d --section=.fast "$ELF" > "$TMP_ASM" 2>/dev/null || {
    echo "FAIL: objdump 反汇编 .fast 失败" >&2
    exit 2
}

# 提取 (调用者, 目标符号, 目标地址)，只保留目标落在 XIP 的项
python3 - "$TMP_ASM" "$TMP_RES" <<'PY'
import re
import sys

asm_path, out_path = sys.argv[1], sys.argv[2]

func_re = re.compile(r'^([0-9a-f]+) <([A-Za-z_][\w]*)>:')
# jal ra,1234 <sym> | jal 1234 <sym> | jalr ... # 8000xxxx <sym>
call_re = re.compile(r'\b(?:jal|jalr)\b.*?([0-9a-f]+)\s*<([A-Za-z_][\w]*)>')

cur = None
rows = []
with open(asm_path, 'r', errors='replace') as fh:
    for line in fh:
        m = func_re.match(line)
        if m:
            cur = m.group(2)
            continue
        if 'jal' not in line:
            continue
        m = call_re.search(line)
        if not m:
            continue
        target = int(m.group(1), 16)
        if target >= 0x80000000 and cur is not None:
            rows.append((cur, m.group(2), target))

with open(out_path, 'w') as fh:
    for row in rows:
        fh.write(f"{row[0]}\t{row[1]}\t0x{row[2]:08x}\n")
PY

if [[ ! -s "$TMP_RES" ]]; then
    echo "PASS: ILM 跨段调用 0 处（.fast 无跳回 XIP）"
    exit 0
fi

bad=0
ok=0
while IFS=$'\t' read -r caller callee addr; do
    allowed=0
    for a in "${ALLOW[@]}"; do
        if [[ "$callee" == "$a" ]]; then
            allowed=1
            break
        fi
    done
    if [[ "$allowed" -eq 1 ]]; then
        ok=$((ok + 1))
    else
        echo "FAIL: ILM 函数 $caller -> XIP $callee ($addr)"
        echo "      修复：给 $callee 加 ATTR_RAMFUNC/APP_ATTR_RAMFUNC，或加入 ALLOW 并写明理由"
        bad=$((bad + 1))
    fi
done < "$TMP_RES"

if [[ "$bad" -ne 0 ]]; then
    echo "ILM 跨段调用: $bad 处未允许（已允许 $ok 处）"
    exit 1
fi

echo "PASS: ILM 跨段调用全部在允许清单内（$ok 处）"
exit 0