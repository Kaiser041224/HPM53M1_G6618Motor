/**
 * @file    foc_math.c
 * @brief   FOC 数学层查表实现（sincos 查找表）
 * @author  Kaiser
 *
 * 背景：工具链的 sinf/cosf 内部走双精度软浮点（反汇编可见 __floatdidf），
 * 而 HPM5361 的 FPU 只有单精度 → 每次调用数千 cycle，直接把 25kHz 电流环
 * 拖到 5kHz。此处改为 256 段查找表 + 线性插值（误差 < 4e-5，约 40 cycle）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "foc_math.h"

#include <math.h>

float foc_sincos_table[FOC_SINCOS_TABLE_SIZE + 1U];
bool foc_math_table_ready;

void foc_math_init(void) {
    if (foc_math_table_ready) {
        return;
    }
    for (uint32_t k = 0U; k <= FOC_SINCOS_TABLE_SIZE; k++) {
        foc_sincos_table[k] =
            sinf(FOC_TWO_PI_F * (float)k / (float)FOC_SINCOS_TABLE_SIZE);
    }
    foc_math_table_ready = true;
}
