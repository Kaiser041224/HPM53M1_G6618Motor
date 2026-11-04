/**
 * @file    app_can_id.c
 * @brief   CAN 节点号解码实现（CANID 电阻网络）
 * @author  Kaiser
 *
 * 阈值表：2026-11-02 台架实测的 8 档原始码中点，固件按**当前装配**取值，
 * 不依赖原理图位序（R75/R76 与原理图对调）。返修后必须重新实测并更新。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_can_id.h"

#include "app_adc.h"
#include "intf_clock.h"

/** 档位数（S1/S2/S3，共 8 档） */
#define APP_CAN_ID_LEVEL_COUNT (8U)
/** 上电采样次数（奇数便于取中值） */
#define APP_CAN_ID_SAMPLE_COUNT (9U)
/** 采样间隔 [ms] */
#define APP_CAN_ID_SAMPLE_PERIOD_MS (1U)
/** 有效原始码下限（0 附近 = 未写入/短路，实测 ID0 = 508） */
#define APP_CAN_ID_MIN_CODE (100U)
/** 有效原始码上限（全 1 附近 = 开路/未写入） */
#define APP_CAN_ID_MAX_CODE (65000U)
/** 允许的样本极差 [LSB]（最紧档间距 1985 LSB，留足裕量） */
#define APP_CAN_ID_MAX_SPREAD (512U)
/** 内点窗口 [LSB]：以中值为中心剔除上电瞬态离群样本 */
#define APP_CAN_ID_INLIER_WINDOW (256U)
/** 有效样本下限 */
#define APP_CAN_ID_MIN_SAMPLES (5U)
/** 上电稳定等待 [ms]：boot 期间三相桥自检仍在开关，先让 ADC1 序列稳定 */
#define APP_CAN_ID_SETTLE_MS (2U)

/* 相邻两档中点（raw，>= 进档）：按**电压从低到高**排列，ID0 以下无阈值 */
static const uint16_t s_can_id_thresholds[APP_CAN_ID_LEVEL_COUNT - 1U] = {
    6120U, 14014U, 19653U, 28120U, 34261U, 36284U, 38487U,
};

/* 电压档位（低→高）→ 拨码二进制 ID：ID = S1<<2 | S2<<1 | S3。
 * 例：拨码 100（仅 S1 闭合，15 kΩ）→ raw≈11733 → 档位 1 → ID 4。
 * 阈值表按电压排序，ID 表按拨码排序，两者不同序，不能合并成一张表。 */
static const uint8_t s_can_id_level_to_id[APP_CAN_ID_LEVEL_COUNT] = {
    0U, /* 000 */
    4U, /* 100 */
    1U, /* 001 */
    5U, /* 101 */
    2U, /* 010 */
    6U, /* 110 */
    3U, /* 011 */
    7U, /* 111 */
};

static uint16_t s_boot_raw = 0xFFFFU;
static uint16_t s_boot_spread = 0xFFFFU;
static uint8_t s_boot_id = APP_CAN_ID_INVALID;

uint8_t app_can_id_decode(uint16_t raw) {
    uint8_t level = 0U;

    if ((raw <= APP_CAN_ID_MIN_CODE) || (raw > APP_CAN_ID_MAX_CODE)) {
        return APP_CAN_ID_INVALID;
    }
    for (uint8_t i = 0U; i < (APP_CAN_ID_LEVEL_COUNT - 1U); i++) {
        if (raw >= s_can_id_thresholds[i]) {
            level++;
        }
    }
    return s_can_id_level_to_id[level];
}

uint8_t app_can_id_read(void) {
    uint16_t samples[APP_CAN_ID_SAMPLE_COUNT];
    uint16_t inliers[APP_CAN_ID_SAMPLE_COUNT];
    uint8_t count = 0U;
    uint8_t inlier_count = 0U;
    uint16_t median;

    /* boot 期间三相桥自检仍在 50% 开关，CANID 是无电容的高阻网络：
     * 上电前几帧可能带离群值，先让 ADC1 序列稳定运行一段再采样。 */
    intf_clock_delay_ms(APP_CAN_ID_SETTLE_MS);

    for (uint8_t i = 0U; i < APP_CAN_ID_SAMPLE_COUNT; i++) {
        uint16_t raw = 0U;

        app_adc_slow_process();
        if (app_adc_get_raw(ADC_CH_V_CANID, &raw) && (raw >= APP_CAN_ID_MIN_CODE)
            && (raw <= APP_CAN_ID_MAX_CODE)) {
            samples[count] = raw;
            count++;
        }
        intf_clock_delay_ms(APP_CAN_ID_SAMPLE_PERIOD_MS);
    }

    s_boot_raw = 0xFFFFU;
    s_boot_spread = 0xFFFFU;
    if (count < APP_CAN_ID_MIN_SAMPLES) {
        s_boot_id = APP_CAN_ID_INVALID;
        return s_boot_id;
    }

    /* 插入排序（样本数固定很小）→ 中值 */
    for (uint8_t i = 1U; i < count; i++) {
        uint16_t key = samples[i];
        uint8_t j = i;

        while ((j > 0U) && (samples[j - 1U] > key)) {
            samples[j] = samples[j - 1U];
            j--;
        }
        samples[j] = key;
    }
    median = samples[count / 2U];

    /* 以中值为中心保留内点：上电瞬态/单帧毛刺不应否决整轮采样。
     * 内点按原序仍在数组中保持有序。 */
    for (uint8_t i = 0U; i < count; i++) {
        uint16_t diff = (samples[i] > median) ? (uint16_t)(samples[i] - median)
                                              : (uint16_t)(median - samples[i]);

        if (diff <= APP_CAN_ID_INLIER_WINDOW) {
            inliers[inlier_count] = samples[i];
            inlier_count++;
        }
    }
    if (inlier_count < APP_CAN_ID_MIN_SAMPLES) {
        s_boot_raw = median;
        s_boot_spread = (uint16_t)(samples[count - 1U] - samples[0U]);
        s_boot_id = APP_CAN_ID_INVALID;
        return s_boot_id;
    }

    median = inliers[inlier_count / 2U];
    s_boot_raw = median;
    s_boot_spread = (uint16_t)(inliers[inlier_count - 1U] - inliers[0U]);
    s_boot_id = (s_boot_spread <= APP_CAN_ID_MAX_SPREAD) ? app_can_id_decode(median)
                                                         : APP_CAN_ID_INVALID;
    return s_boot_id;
}

uint16_t app_can_id_get_boot_raw(void) { return s_boot_raw; }

uint16_t app_can_id_get_boot_spread(void) { return s_boot_spread; }

uint8_t app_can_id_get_boot_id(void) { return s_boot_id; }
