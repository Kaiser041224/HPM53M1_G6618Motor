#ifndef ENCODER_H
#define ENCODER_H

/**
 * @file    encoder.h
 * @brief   离线验证用桩：替代上游 Hw/encoder.h，只为让 Foc/ 纯算法层脱离硬件编译
 * @author  Kaiser
 *
 * 上游 foc_vernier.c 只从 encoder.h 取 ENCODER_CPR（编码器每圈计数）。
 * 这里按上游 Hw/encoder.h 的定义复制该宏，不引入任何硬件依赖。
 * 注意：本文件仅用于 Doc/ref_code/host_test 的宿主机编译，不参与固件构建。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stdint.h>
#include "config.h"

/* MT6701 = 14 bit（上游 CFG_ENCODER_TYPE 默认即为 MT6701） */
#define ENCODER_CPR           16384U
#define ENCODER_READ_INVALID  0xFFFFU

#endif /* ENCODER_H */
