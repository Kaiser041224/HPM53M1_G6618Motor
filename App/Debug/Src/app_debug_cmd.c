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
 *
 * 说明：零点为软件方案（app_param），不消耗编码器 MTP。
 */

#include "app_debug_cmd.h"

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

        default:
            break;
        }
    }
}
