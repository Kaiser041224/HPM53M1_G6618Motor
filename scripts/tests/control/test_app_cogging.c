/**
 * @file    test_app_cogging.c
 * @brief   齿槽补偿角表、使能、挂起和 q 电流前馈测试
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "test_util.h"

extern void mock_params_reset(void);

static void cogging_tables_constant(app_cogging_tables_t* tables, int16_t rotor_value,
                                    int16_t output_value) {
    *tables = (app_cogging_tables_t){0};
    for (uint16_t i = 0U; i < ALGO_COGGING_ROTOR_POINTS; i++) {
        tables->rotor[i] = rotor_value;
    }
    for (uint16_t i = 0U; i < ALGO_COGGING_OUTPUT_POINTS; i++) {
        tables->output[i] = output_value;
    }
    tables->rotor_valid = true;
    tables->output_valid = true;
    tables->rotor_stats.valid = true;
    tables->output_stats.valid = true;
}

void test_app_cogging(void) {
    app_cogging_tables_t tables;
    app_cogging_snapshot_t snapshot;

    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = 0.1f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_cogging_init();

    /* 没有表时不能开启，注水为 0。 */
    CHECK(app_cogging_set_master(true) != 0);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);

    cogging_tables_constant(&tables, 100, 100);
    CHECK(app_cogging_set_tables(&tables) == 0);
    CHECK(app_cogging_set_master(true) == 0);
    CHECK(app_cogging_set_rotor_enable(true) == 0);
    CHECK(app_cogging_set_output_enable(true) == 0);
    app_cogging_set_output_angle(0.0f, true);
    app_cogging_set_motion_active(true);

    /* 转子 100LSB×0.0005/0.1 = 0.5A；输出 100LSB×0.001/(50×0.1)=0.02A。 */
    CHECK_NEAR(app_cogging_iq(0.0f), 0.52f, 1e-5f);
    CHECK_NEAR(app_cogging_iq(3.0f), 0.52f, 1e-5f);

    /* 总开关、表开关、挂起、速度限幅、失效输出角均能独立关断。 */
    app_cogging_set_master(false);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
    CHECK(app_cogging_set_master(true) == 0);

    app_cogging_suspend(true);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
    app_cogging_suspend(false);

    app_cogging_set_speed_limited(true);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
    app_cogging_set_speed_limited(false);

    app_cogging_set_motion_active(false);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
    app_cogging_set_motion_active(true);

    app_cogging_set_output_angle(0.0f, false);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.5f, 1e-5f);
    app_cogging_set_output_angle(0.0f, true);

    CHECK(app_cogging_set_rotor_enable(false) == 0);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.02f, 1e-5f);
    CHECK(app_cogging_set_rotor_enable(true) == 0);

    CHECK(app_cogging_set_output_enable(false) == 0);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.5f, 1e-5f);
    CHECK(app_cogging_set_output_enable(true) == 0);

    CHECK(app_cogging_set_gain(0.5f) == 0);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.26f, 1e-5f);
    CHECK(app_cogging_set_gain(1.0f) == 0);

    app_cogging_get_snapshot(&snapshot);
    CHECK(snapshot.master_enabled);
    CHECK(snapshot.rotor_enabled);
    CHECK(snapshot.output_enabled);
    CHECK(snapshot.rotor_valid);
    CHECK(snapshot.output_valid);
    CHECK(!snapshot.suspended);
    CHECK(!snapshot.speed_limited);

    app_cogging_clear_tables();
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
}
void test_app_cogging_defaults(void) {
    app_cogging_tables_t tables;
    app_cogging_snapshot_t snapshot;

    mock_params_reset();
    app_motor_params_mutable()->kt_nm_per_a = 0.1f;
    app_motor_params_mutable()->reducer_ratio = 50.0f;
    app_software_params_t* software = app_software_params_mutable();
    software->cogging.enable = 1U;
    software->cogging.rotor_enable = 1U;
    software->cogging.output_enable = 0U;
    software->cogging.gain = 0.5f;

    app_cogging_init();
    cogging_tables_constant(&tables, 100, 100);
    CHECK(app_cogging_set_tables(&tables) == 0);
    app_cogging_set_motion_active(true);
    app_cogging_set_output_angle(0.0f, true);
    app_cogging_apply_defaults();
    app_cogging_get_snapshot(&snapshot);
    CHECK(snapshot.master_enabled);
    CHECK(snapshot.rotor_enabled);
    CHECK(!snapshot.output_enabled);
    CHECK_NEAR(snapshot.gain, 0.5f, 1e-6f);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.25f, 1e-5f);

    /* master off：表选择保留，但补偿量为 0。 */
    software->cogging.enable = 0U;
    app_cogging_apply_defaults();
    app_cogging_get_snapshot(&snapshot);
    CHECK(!snapshot.master_enabled);
    CHECK(snapshot.rotor_enabled);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);

    /* YAML 选择 output 表：master 仍关闭，手动 cogging on 才生效。 */
    software->cogging.output_enable = 1U;
    software->cogging.gain = 1.0f;
    app_cogging_apply_defaults();
    app_cogging_get_snapshot(&snapshot);
    CHECK(!snapshot.master_enabled);
    CHECK(snapshot.rotor_enabled);
    CHECK(snapshot.output_enabled);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.0f, 1e-6f);
    CHECK(app_cogging_set_master(true) == 0);
    CHECK_NEAR(app_cogging_iq(0.0f), 0.52f, 1e-5f);

    /* 表失效时 boot 默认必须保持总开关关闭。 */
    app_cogging_clear_tables();
    app_cogging_apply_defaults();
    app_cogging_get_snapshot(&snapshot);
    CHECK(!snapshot.master_enabled);
    CHECK(!snapshot.rotor_enabled);
    CHECK(!snapshot.output_enabled);
}
