/**
 * @file    test_algo_inertia.c
 * @brief   惯量辨识结果计算契约测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_inertia.h"
#include "test_util.h"

void test_algo_inertia(void) {
    algo_inertia_result_t out;

    /* J_total = 2τ / (α+ - α-) = 2/20 = 0.1 kg·m²；反射转子 0.0575 → 负载 0.0425。 */
    CHECK(algo_inertia_calculate(1.0f, 10.0f, -10.0f, 2.3e-5f, 50.0f, &out) == 0);
    CHECK_NEAR(out.j_total_kgm2, 0.1f, 1e-6f);
    CHECK_NEAR(out.j_rotor_reflected_kgm2, 0.0575f, 1e-6f);
    CHECK_NEAR(out.j_load_kgm2, 0.0425f, 1e-6f);
    CHECK(!out.clamped);

    /* 测量总惯量小于已知转子反射：负载钳到0，并标记 clamped。 */
    CHECK(algo_inertia_calculate(1.0f, 20.0f, -20.0f, 2.3e-5f, 50.0f, &out) == 0);
    CHECK_NEAR(out.j_total_kgm2, 0.05f, 1e-6f);
    CHECK_NEAR(out.j_load_kgm2, 0.0f, 1e-6f);
    CHECK(out.clamped);

    /* 退化/非法输入必须拒绝，不得把 NaN 写进参数。 */
    CHECK(algo_inertia_calculate(1.0f, 10.0f, 10.0f, 2.3e-5f, 50.0f, &out) != 0);
    CHECK(algo_inertia_calculate(0.0f, 10.0f, -10.0f, 2.3e-5f, 50.0f, &out) != 0);
    CHECK(algo_inertia_calculate(NAN, 10.0f, -10.0f, 2.3e-5f, 50.0f, &out) != 0);
    CHECK(algo_inertia_calculate(1.0f, 10.0f, -10.0f, NAN, 50.0f, &out) != 0);
    CHECK(algo_inertia_calculate(1.0f, 10.0f, -10.0f, 2.3e-5f, 0.0f, &out) != 0);
    CHECK(algo_inertia_calculate(1.0f, 10.0f, -10.0f, 2.3e-5f, 50.0f, NULL) != 0);

    /* 多点综合：中位数抵抗一个高电流离群点，MAD/极差用于质量判定。 */
    const float points[] = {0.115f, 0.116f, 0.114f, 0.150f, 0.118f, 0.113f};
    const bool valid[] = {true, true, true, true, true, true};
    float median = 0.0f;
    float mad = 0.0f;
    float spread = 0.0f;
    uint16_t valid_count = 0U;
    CHECK(algo_inertia_aggregate(
              points, valid, 6U, 2.3e-5f, 50.0f, &out, &median, &mad, &spread,
              &valid_count)
          == 0);
    CHECK(valid_count == 6U);
    CHECK(median > 0.113f && median < 0.118f);
    CHECK_NEAR(out.j_total_kgm2, median, 1e-7f);
    CHECK(out.j_load_kgm2 > 0.05f && out.j_load_kgm2 < 0.07f);
    CHECK(mad > 0.0f && mad < 0.01f);
    CHECK(spread > 0.0f && spread < 0.4f);

    /* 无效点被忽略；全无效必须拒绝。 */
    const bool partly_valid[] = {true, false, true, false, true, false};
    CHECK(algo_inertia_aggregate(
              points, partly_valid, 6U, 2.3e-5f, 50.0f, &out, &median, &mad, &spread,
              &valid_count)
          == 0);
    CHECK(valid_count == 3U);
    const bool all_invalid[] = {false, false, false, false, false, false};
    CHECK(algo_inertia_aggregate(
              points, all_invalid, 6U, 2.3e-5f, 50.0f, &out, &median, &mad, &spread,
              &valid_count)
          != 0);
}
