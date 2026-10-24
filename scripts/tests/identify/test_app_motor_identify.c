/**
 * @file    test_app_motor_identify.c
 * @brief   电角度自校准编排（模块隔离）主机自测
 * @author  Kaiser
 *
 * 契约：
 *   1) 只经 FOC 校准端口借用电流环：begin → 强制角激励 → 试用零点验证 → end（恰好一次）
 *   2) 结果只作为数据：完成后不改运行参数、不改 FOC 运行零点；apply 才生效
 *   3) 任何失败/中止路径都调用 end（FOC 回 READY，试用零点作废）
 *   4) 自带过流中止（不依赖全局保护动作开关）
 *   5) save/restore：FOC 关闭才可保存；极对数不符的记录拒绝恢复
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_util.h"

#include "app_foc.h"
#include "app_motor_identify.h"
#include "app_motor_params.h"

extern void mock_reset(float off_true, float dir_true);
extern void mock_store_reset(void);
extern void mock_plant_step(void);
extern app_foc_state_t g_mock_foc_state;
extern int g_mock_calib_begin_count;
extern int g_mock_calib_end_count;
extern int g_mock_apply_count;
extern float g_mock_applied_offset;
extern float g_mock_applied_dir;
extern float g_mock_current_scale;
extern bool g_mock_probe_stuck;
extern float g_mock_probe_min_a;
extern bool g_mock_snap_invalid;
extern uint32_t g_mock_enc_errors;
extern int g_mock_store_count;

/** 运行至结束（或上限 tick），返回经过的 tick 数 */
static int run_until_idle(int max_ticks) {
    int n;

    for (n = 0; n < max_ticks; n++) {
        mock_plant_step();
        app_motor_identify_run_once();
        if (!app_motor_identify_is_active()) {
            break;
        }
    }
    return n;
}

/** 推进固定 tick 数（不判结束） */
static void run_ticks(int ticks) {
    for (int n = 0; n < ticks; n++) {
        mock_plant_step();
        app_motor_identify_run_once();
    }
}

void test_app_motor_identify(void) {
    app_motor_identify_result_t res;
    const float off_true = 1.234f;

    /* ---- 1) 完整成功流程：数据产出，不自动生效 ---- */
    mock_reset(off_true, 1.0f);
    CHECK(app_motor_identify_apply() != 0); /* 无结果：拒绝 */
    CHECK(app_motor_identify_start() == 0);
    CHECK(app_motor_identify_is_active());
    CHECK(g_mock_calib_begin_count == 1);
    CHECK(g_mock_foc_state == APP_FOC_STATE_CALIB);
    CHECK(app_motor_identify_start() != 0); /* 进行中：拒绝重入 */

    CHECK(run_until_idle(60000) < 60000);
    app_motor_identify_get_result(&res);
    CHECK(res.done);
    CHECK(!res.failed);
    CHECK_NEAR(res.offset_rad, off_true, 0.02f);
    CHECK_NEAR(res.direction, 1.0f, 1e-6f);
    CHECK(res.quality > 0.95f);
    CHECK(res.probe_travel_rad * res.direction >= 0.05f);
    CHECK((res.probe_fwd_rad * res.direction >= 0.05f)
          || (res.probe_rev_rad * res.direction <= -0.05f));
    CHECK(g_mock_calib_end_count == 1);     /* 结束必经 end */
    CHECK(g_mock_foc_state == APP_FOC_STATE_READY);
    CHECK(g_mock_apply_count == 0);         /* 不自动应用 */
    CHECK_NEAR(app_motor_params_current()->encoder.electrical_offset_rad, 0.0f, 1e-6f);
    CHECK(app_motor_params_current()->encoder.electrical_offset_valid == 0U);

    /* apply：写运行参数 + FOC 运行零点 */
    CHECK(app_motor_identify_apply() == 0);
    CHECK(g_mock_apply_count == 1);
    CHECK_NEAR(g_mock_applied_offset, res.offset_rad, 1e-6f);
    CHECK_NEAR(g_mock_applied_dir, 1.0f, 1e-6f);
    CHECK_NEAR(app_motor_params_current()->encoder.electrical_offset_rad, res.offset_rad, 1e-6f);
    CHECK(app_motor_params_current()->encoder.electrical_offset_valid == 1U);

    /* ---- 2) 反方向 ---- */
    mock_reset(off_true, -1.0f);
    CHECK(app_motor_identify_start() == 0);
    CHECK(run_until_idle(60000) < 60000);
    app_motor_identify_get_result(&res);
    CHECK(res.done);
    CHECK_NEAR(res.direction, -1.0f, 1e-6f);
    CHECK_NEAR(res.offset_rad, off_true, 0.02f);

    /* ---- 3) 前置：FOC 未使能 → 拒绝，不触碰端口 ---- */
    mock_reset(off_true, 1.0f);
    g_mock_foc_state = APP_FOC_STATE_OFF;
    CHECK(app_motor_identify_start() != 0);
    CHECK(g_mock_calib_begin_count == 0);
    CHECK(!app_motor_identify_is_active());

    /* ---- 4) 用户中止 ---- */
    mock_reset(off_true, 1.0f);
    CHECK(app_motor_identify_start() == 0);
    run_ticks(100);
    app_motor_identify_abort();
    app_motor_identify_get_result(&res);
    CHECK(!app_motor_identify_is_active());
    CHECK(res.failed);
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_ABORTED);
    CHECK(g_mock_calib_end_count == 1);
    CHECK(app_motor_identify_apply() != 0); /* 失败结果不可应用 */

    /* ---- 5) FOC 被外部关闭 → STATE 失败 ---- */
    mock_reset(off_true, 1.0f);
    CHECK(app_motor_identify_start() == 0);
    run_ticks(50);
    g_mock_foc_state = APP_FOC_STATE_OFF;
    run_ticks(2);
    app_motor_identify_get_result(&res);
    CHECK(!app_motor_identify_is_active());
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_STATE);

    /* ---- 6) 过流：实测 |i| 持续超限 → OVERCURRENT（模块自带，不依赖全局保护开关） ---- */
    mock_reset(off_true, 1.0f);
    g_mock_current_scale = 4.0f; /* 2A 给定 → 8A 实测 */
    CHECK(app_motor_identify_start() == 0);
    CHECK(run_until_idle(100) < 100);
    app_motor_identify_get_result(&res);
    CHECK(res.failed);
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_OVERCURRENT);
    CHECK(g_mock_calib_end_count == 1);

    /* ---- 7) 编码器错误计数增长 → ENCODER ---- */
    mock_reset(off_true, 1.0f);
    CHECK(app_motor_identify_start() == 0);
    run_ticks(10);
    g_mock_enc_errors++;
    run_ticks(2);
    app_motor_identify_get_result(&res);
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_ENCODER);
    CHECK(!app_motor_identify_is_active());

    /* ---- 8) 验证失败（探针无行程）：VERIFY 失败、试用零点作废、不可应用 ---- */
    mock_reset(off_true, 1.0f);
    g_mock_probe_stuck = true;
    CHECK(app_motor_identify_start() == 0);
    CHECK(run_until_idle(60000) < 60000);
    app_motor_identify_get_result(&res);
    CHECK(res.failed);
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_VERIFY);
    CHECK_NEAR(res.probe_current_a, 4.0f, 1e-6f);
    CHECK(g_mock_calib_end_count == 1);
    CHECK(g_mock_apply_count == 0);
    CHECK(app_motor_identify_apply() != 0);

    /* ---- 8b) RUN 阶段电流快照不可信（电流环保护路径）→ STATE 失败，不以陈旧电流继续 ---- */
    mock_reset(off_true, 1.0f);
    CHECK(app_motor_identify_start() == 0);
    run_ticks(20);
    g_mock_snap_invalid = true;
    run_ticks(2);
    app_motor_identify_get_result(&res);
    CHECK(!app_motor_identify_is_active());
    CHECK(res.fail_reason == APP_IDENTIFY_REASON_STATE);

    /* ---- 8c) 有未应用的成功结果时 save 拒绝（防止误把旧值当新结果落盘） ---- */
    mock_reset(off_true, 1.0f);
    mock_store_reset();
    CHECK(app_motor_identify_start() == 0);
    CHECK(run_until_idle(60000) < 60000);
    app_motor_identify_get_result(&res);
    CHECK(res.done);
    g_mock_foc_state = APP_FOC_STATE_OFF;
    CHECK(app_motor_identify_save() == APP_IDENTIFY_SAVE_PENDING); /* 未 apply */
    CHECK(g_mock_store_count == 0);
    CHECK(app_motor_identify_apply() == 0);                        /* OFF 下 apply 允许 */
    CHECK(app_motor_identify_save() == 0);                         /* 已 apply：保存结果 */
    CHECK(g_mock_store_count == 1);
    app_motor_params_mutable()->encoder.electrical_offset_rad = 0.0f;
    app_motor_params_mutable()->encoder.electrical_offset_valid = 0U;
    CHECK(app_motor_identify_restore() == 0);
    CHECK_NEAR(app_motor_params_current()->encoder.electrical_offset_rad, res.offset_rad, 1e-6f);
    CHECK(app_motor_params_current()->encoder.electrical_offset_valid == 1U);

    /* ---- 9) save / restore ---- */
    mock_reset(off_true, 1.0f);
    mock_store_reset();
    app_motor_params_mutable()->encoder.electrical_offset_rad = 2.5f;
    app_motor_params_mutable()->encoder.direction = -1.0f;
    CHECK(app_motor_identify_save() != 0); /* FOC 非 OFF：拒绝写 flash */
    CHECK(g_mock_store_count == 0);
    g_mock_foc_state = APP_FOC_STATE_OFF;
    CHECK(app_motor_identify_save() == 0);
    CHECK(g_mock_store_count == 1);
    CHECK(app_motor_params_current()->encoder.electrical_offset_valid == 1U);

    app_motor_params_mutable()->encoder.electrical_offset_rad = 0.0f;
    app_motor_params_mutable()->encoder.direction = 1.0f;
    CHECK(app_motor_identify_restore() == 0);
    CHECK_NEAR(app_motor_params_current()->encoder.electrical_offset_rad, 2.5f, 1e-6f);
    CHECK_NEAR(app_motor_params_current()->encoder.direction, -1.0f, 1e-6f);

    /* 极对数不符的记录：拒绝恢复，参数不变 */
    app_motor_params_mutable()->pole_pairs = 7U;
    app_motor_params_mutable()->encoder.electrical_offset_rad = 0.0f;
    CHECK(app_motor_identify_restore() < 0);
    CHECK_NEAR(app_motor_params_current()->encoder.electrical_offset_rad, 0.0f, 1e-6f);

    /* 无记录：返回"未找到"（>0），参数不变 */
    mock_store_reset();
    app_motor_params_mutable()->pole_pairs = 10U;
    CHECK(app_motor_identify_restore() > 0);
}

/** @brief 高静摩擦：2A 探针不动，应自动升到 4A 后通过 */
void test_app_motor_identify_adaptive_probe(void) {
    app_motor_identify_result_t res;

    mock_reset(1.234f, 1.0f);
    g_mock_probe_min_a = 3.0f; /* 2A 不足，4A 足够 */
    CHECK(app_motor_identify_start() == 0);
    CHECK(run_until_idle(60000) < 60000);
    app_motor_identify_get_result(&res);
    CHECK(res.done);
    CHECK(!res.failed);
    CHECK_NEAR(res.probe_current_a, 4.0f, 1e-6f);
    CHECK(res.probe_travel_rad * res.direction >= 0.05f);
    CHECK((res.probe_fwd_rad * res.direction >= 0.05f)
          || (res.probe_rev_rad * res.direction <= -0.05f));
}
