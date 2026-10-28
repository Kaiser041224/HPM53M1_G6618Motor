#!/usr/bin/env bash
# 布局守卫：断言"必须在 ILM 常驻"的符号不落在 XIP flash。
#
# 背景：慢保护链（fault / analog / rms）曾在 XIP，实测 ADC1 侧 isr_max=7127 cycle
# （14.85µs 取指风暴）。ILM 常驻是"慢保护进任何高频上下文"的硬前置。
# 本脚本把该约束变成可自动检查的测试，防止回归。
#
# 用法：scripts/tests/layout/check_layout.sh [elf]
#   默认 output/HPM53M1_G6618Motor.elf（需先 make artifacts）
# 退出码：0 = 全部在 ILM（0x0000xxxx）；非 0 = 有符号落在 XIP（0x8xxxxxxx）
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
ELF="${1:-$ROOT/output/HPM53M1_G6618Motor.elf}"

NM="${GNURISCV_TOOLCHAIN_PATH:-/opt/riscv32-gnu-toolchain-elf-bin}/bin/riscv32-unknown-elf-nm"

# 必须在 ILM 的符号（分层理由见 docs/superpowers/plans/2026-10-27-protection-chain-v3.md）
SYMBOLS=(
    app_fault_tick
    app_fault_process
    app_fault_on_wdog
    app_fault_clear
    app_analog_signal_read_all
    algo_rms_step_impl
    algo_rms_get_rms_impl
    algo_rms_update_sq_impl
    algo_rms_calc_mean_sq
    algo_rms_rebuild_sum_sq
    # Task 3~7 新增：30kHz ISR 路径（WDOG 回调 → fault_raise；FOC 门控 → get_policy/限幅）
    app_fault_raise
    app_fault_get_policy
    app_fault_get_action
    app_fault_get_limit_scale
    app_fault_actions_enabled
    fault_publish
    fault_raise
    fault_eval_state
    fault_code_bit
    fault_effective_active
    fault_effective_latched
    adc_get_seq_frames
    # 2026-10-28 新增：多圈/MIT 实时路径（15kHz motion tick → 位置/P 与阻抗）
    algo_motion_continuous_step
    algo_motion_position
    algo_motion_position_cont
    algo_mit_step
    mit_finite
    mit_approach
    # 2026-10-28 第二批：消除「ILM 内跳回 XIP」——30kHz/15kHz 热路径
    algo_cogging_interp_i16
    app_cogging_iq
    app_cogging_set_output_angle
    app_cogging_set_motion_active
    app_cogging_set_speed_limited
    app_foc_stop_pending
    app_foc_request_stop
    app_foc_revoke_current
    # 2026-10-28 第二批：1kHz 保护链（fault/ADC/编码器/桥臂）
    fault_finite
    fault_snapshot_now
    fault_conditions_ok
    fault_reset_all
    app_adc_get_sequence
    app_adc_wdog_reenable
    app_encoder_get_error_count
    app_3phase_inverter_is_enabled
    # 2026-10-28 第二批：故障停机路径 + flash ILM 实现的同段辅助
    app_gpio_set
    intf_gpio_set_level
    intf_adc_wdog_reenable
    hpm_gpio_set_level
    get_gpio_base
    get_gpio_port_index
    adc_wdog_reenable
    flash_addr_in_range
    flash_dcache_invalidate_aligned
    ntc_resistance_from_volts
)

if [[ ! -x "$NM" ]]; then
    echo "FAIL: nm 不可执行: $NM（设 GNURISCV_TOOLCHAIN_PATH）" >&2
    exit 2
fi
if [[ ! -f "$ELF" ]]; then
    echo "FAIL: ELF 不存在: $ELF（先 make artifacts）" >&2
    exit 2
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
"$NM" "$ELF" > "$TMP" 2>/dev/null || { echo "FAIL: nm 读取失败" >&2; exit 2; }

bad=0
ok=0
for sym in "${SYMBOLS[@]}"; do
    addr="$(grep -E "[0-9a-f]{8} [A-Za-z] ${sym}\$" "$TMP" | head -1 | awk '{print $1}')"
    if [[ -z "$addr" ]]; then
        printf '  %-30s %s\n' "$sym" "MISSING（符号不存在：改名或未编译？）"
        bad=$((bad + 1))
        continue
    fi
    if [[ "$addr" == 0000* ]]; then
        ok=$((ok + 1))
    else
        printf '  %-30s %s  ← 落在 XIP（应在 ILM 0x0000xxxx）\n' "$sym" "$addr"
        bad=$((bad + 1))
    fi
done

total=${#SYMBOLS[@]}
if [[ "$bad" -eq 0 ]]; then
    echo "PASS: ILM 布局 ${ok}/${total}（elf: $(basename "$ELF")）"
    exit 0
fi

echo "FAIL: ILM 布局 ${ok}/${total}，${bad} 个符号不在 ILM" >&2
exit 1