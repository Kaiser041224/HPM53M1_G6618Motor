/*
 * Debug Cmd - 调试控制台单字符命令（UART / USB 共用）
 *
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 命令（在 UART 或 USB 终端直接输入单字符）：
 *   z = 设置转子零点（记录当前原始值到 flash 参数区）
 *   o = 设置出轴零点
 *   c = 清除两路零点
 *   i = 打印零点与当前位置
 *   1/2/3 = 仅驱动三相半桥的 U/V/W 相（25kHz/50%，用于故障定位）
 *   a = 三相全开；0 = 三相全关（含 12V）
 *   r = 开环旋转启停（V/F）；+/- = 电频率 ±0.5Hz；m/M = 调制比 ∓/±1%
 *   d = ADC 全通道表（raw / mV / 物理量）；p = ADC 诊断（PMT 完成率等）
 *   k = 触发延时预设循环（100/250/500/1000/2000 ns）；n = 电流零点标定
 *   f = 故障保护状态（状态机/故障码/计数/快照）；F = 清除锁存
 *
 * 说明：零点为软件方案（app_param），不消耗编码器 MTP。
 */

#include "app_debug_cmd.h"

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_debug_adc.h"
#include "app_debug_fault.h"
#include "app_debug_inverter.h"
#include "app_debug_motor.h"
#include "app_debug_rtt.h"
#include "app_encoder.h"

#include <stdint.h>

static void cmd_print_info(void)
{
    uint16_t zero_rotor = 0U, zero_output = 0U;
    uint16_t pos_rotor = 0U, pos_output = 0U;

    (void) app_encoder_get_zero(APP_ENCODER_ROTOR, &zero_rotor);
    (void) app_encoder_get_zero(APP_ENCODER_OUTPUT, &zero_output);
    (void) app_encoder_read_position(APP_ENCODER_ROTOR, &pos_rotor);
    (void) app_encoder_read_position(APP_ENCODER_OUTPUT, &pos_output);

    app_debug_printf(
        "[CMD] param=%s | zero: rotor=0x%04X output=0x%04X | pos: rotor=0x%04X output=0x%04X\r\n",
        app_encoder_is_param_loaded() ? "valid" : "default", (unsigned) zero_rotor, (unsigned) zero_output,
        (unsigned) pos_rotor, (unsigned) pos_output);
}

void app_debug_cmd_handle(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return;
    }

    for (size_t i = 0U; i < len; i++) {
        switch (data[i]) {
        case 'z':
            if (app_encoder_set_zero(APP_ENCODER_ROTOR) == 0) {
                app_debug_printf("[CMD] rotor zero set (saved to flash)\r\n");
            } else {
                app_debug_printf("[CMD] rotor zero FAILED\r\n");
            }
            cmd_print_info();
            break;

        case 'o':
            if (app_encoder_set_zero(APP_ENCODER_OUTPUT) == 0) {
                app_debug_printf("[CMD] output zero set (saved to flash)\r\n");
            } else {
                app_debug_printf("[CMD] output zero FAILED\r\n");
            }
            cmd_print_info();
            break;

        case 'c':
            (void) app_encoder_clear_zero(APP_ENCODER_ROTOR);
            (void) app_encoder_clear_zero(APP_ENCODER_OUTPUT);
            app_debug_printf("[CMD] zero cleared (rotor + output)\r\n");
            cmd_print_info();
            break;

        case 'i':
            cmd_print_info();
            break;

        /* 三相逆变桥逐相控制（bring-up 故障定位/相序确认；先停旋转避免状态冲突） */
        case '1':
            app_debug_motor_stop();
            app_debug_inverter_set_output(0x1U); /* 仅 U */
            break;
        case '2':
            app_debug_motor_stop();
            app_debug_inverter_set_output(0x2U); /* 仅 V */
            break;
        case '3':
            app_debug_motor_stop();
            app_debug_inverter_set_output(0x4U); /* 仅 W */
            break;
        case 'a':
            app_debug_motor_stop();
            app_debug_inverter_set_output(0x7U); /* 三相全开 */
            break;
        case '0':
            app_debug_motor_stop();              /* 安全：先停旋转（含归零矢量） */
            app_debug_inverter_set_output(0x0U); /* 全关（含 12V） */
            break;

        /* ADC 采样链 */
        case 'd':
            app_debug_adc_dump_channels();
            break;
        case 'p':
            app_debug_adc_dump_diag();
            break;
        case 'f':
            app_debug_fault_dump();
            break;
        case 'F':
            app_debug_fault_clear();
            break;
        case 'k': {
            /* 触发延时预设循环：验证采样点是否落在低侧导通窗口内 */
            static const uint32_t presets[] = {100U, 250U, 500U, 1000U, 2000U};
            static uint8_t idx;
            uint32_t delay_ns = presets[idx];

            idx = (uint8_t) ((idx + 1U) % (sizeof(presets) / sizeof(presets[0])));
            if (app_adc_set_trigger_delay_ns(delay_ns) == 0) {
                app_debug_printf("[CMD] ADC trigger delay = %u ns\r\n", (unsigned) delay_ns);
            } else {
                app_debug_printf("[CMD] ADC trigger delay set FAILED\r\n");
            }
            break;
        }
        case 'n':
            if (app_analog_signal_calibrate_offsets() == 0) {
                app_debug_printf("[CMD] ADC zero calibration: OK\r\n");
            } else {
                app_debug_printf("[CMD] ADC zero calibration: FAILED (no current required)\r\n");
            }
            break;

        /* 开环旋转自检（V/F） */
        case 'r':
            app_debug_motor_rotation_toggle();
            break;
        case '+':
            app_debug_motor_freq_step(1);
            break;
        case '-':
            app_debug_motor_freq_step(-1);
            break;
        case 'm':
            app_debug_motor_mod_step(-1);
            break;
        case 'M':
            app_debug_motor_mod_step(1);
            break;

        default:
            break;
        }
    }
}
