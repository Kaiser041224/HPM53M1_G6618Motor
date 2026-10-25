/**
 * @file    test_algo_cogging.c
 * @brief   齿槽角表分箱、F/R 合成、插值与统计契约测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_cogging.h"
#include "test_util.h"

void test_algo_cogging(void) {
    algo_cogging_bin_t bin = {0};
    float torque = 0.0f;

    /* 正/反向 q 电流相加消除恒定摩擦，输出侧再乘减速比。 */
    algo_cogging_bin_reset(&bin);
    algo_cogging_bin_add(&bin, 2.5f, 1);
    algo_cogging_bin_add(&bin, -1.5f, -1);
    CHECK(algo_cogging_bin_torque(&bin, 0.1f, 50.0f, false, &torque) == 0);
    CHECK_NEAR(torque, 0.05f, 1e-6f); /* (2.5 - 1.5)/2 * 0.1 */
    CHECK(algo_cogging_bin_torque(&bin, 0.1f, 50.0f, true, &torque) == 0);
    CHECK_NEAR(torque, 2.5f, 1e-6f); /* 0.05 * 50 */

    /* 只有一个方向时不得生成表值。 */
    algo_cogging_bin_reset(&bin);
    algo_cogging_bin_add(&bin, 1.0f, 1);
    CHECK(algo_cogging_bin_torque(&bin, 0.1f, 50.0f, false, &torque) != 0);

    /* 环形插值：4 点表跨 0/2π。 */
    const int16_t table[] = {0, 100, 0, -100};
    CHECK_NEAR(algo_cogging_interp_i16(table, 4, 0.001f, 0.0f), 0.0f, 1e-6f);
    CHECK_NEAR(algo_cogging_interp_i16(table, 4, 0.001f, 1.5707963f), 0.1f, 1e-5f);
    CHECK_NEAR(algo_cogging_interp_i16(table, 4, 0.001f, 3.1415926f), 0.0f, 1e-5f);
    CHECK_NEAR(algo_cogging_interp_i16(table, 4, 0.001f, 4.7123890f), -0.1f, 1e-5f);
    CHECK_NEAR(algo_cogging_interp_i16(table, 4, 0.001f, 6.2831853f), 0.0f, 1e-5f);

    /* 去直流、缩放、无效箱插值，并统计 RMS/峰峰值。 */
    const float raw[] = {1.0f, 1.1f, 0.0f, 0.9f};
    const bool valid[] = {true, true, false, true};
    int16_t out[4] = {0};
    algo_cogging_stats_t stats = {0};
    CHECK(algo_cogging_finalize(raw, valid, 4U, 0.001f, out, &stats) == 0);
    CHECK(stats.valid);
    CHECK(stats.valid_bins == 3U);
    CHECK_NEAR(stats.mean_nm, 1.0f, 1e-6f);
    CHECK(stats.peak_to_peak_nm > 0.19f && stats.peak_to_peak_nm < 0.21f);
    CHECK(stats.rms_nm > 0.0f);
    CHECK(out[0] == 0);
    CHECK(out[1] == 100);
    CHECK(out[3] == -100);

    /* 参数非法/有效箱不足必须拒绝。 */
    CHECK(algo_cogging_finalize(NULL, valid, 4U, 0.001f, out, &stats) != 0);
    const bool none_valid[] = {false, false, false, false};
    CHECK(algo_cogging_finalize(raw, none_valid, 4U, 0.001f, out, &stats) != 0);

    /* 直接由分箱生成表，省去标定侧 torque/valid 临时数组。 */
    algo_cogging_bin_t bins[4] = {0};
    algo_cogging_bin_add(&bins[0], 2.0f, 1);
    algo_cogging_bin_add(&bins[0], -2.0f, -1);
    algo_cogging_bin_add(&bins[1], 2.1f, 1);
    algo_cogging_bin_add(&bins[1], -1.9f, -1);
    algo_cogging_bin_add(&bins[2], 9.0f, 1); /* 只有正向，无效 */
    algo_cogging_bin_add(&bins[3], 1.9f, 1);
    algo_cogging_bin_add(&bins[3], -2.1f, -1);
    CHECK(algo_cogging_finalize_bins(bins, 4U, 0.1f, 50.0f, false, 0.001f, out, &stats) == 0);
    CHECK(stats.valid);
    CHECK(stats.valid_bins == 3U);
    CHECK(out[0] == 0);
    CHECK(out[1] == 10);
    CHECK(out[3] == -10);
}
