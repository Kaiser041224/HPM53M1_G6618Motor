/**
 * @file    test_main.c
 * @brief   参数持久化主机自测：带代次记录读写 + 出厂值回落
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_motor_params.h"
#include "app_param.h"
#include "app_param_record.h"
#include "app_software_params.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_fails;

#define CHECK(cond)                                                \
    do {                                                           \
        g_checks++;                                                \
        if (!(cond)) {                                             \
            g_fails++;                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

void mock_flash_reset(void);

/** @brief 记录读写：回读一致；代次/长度不符或非法入参整条忽略，不改内存 */
static void test_param_record(void) {
    typedef struct {
        float a;
        float b;
    } payload_t;
    payload_t out, untouched, in = {1.5f, -2.25f};
    mock_flash_reset();
    memset(&out, 0x5A, sizeof(out));
    untouched = out;
    CHECK(app_param_record_load(0x54455354U, &out, sizeof(out), 3) != 0);
    CHECK(memcmp(&out, &untouched, sizeof(out)) == 0); /* 无记录：内存不被改动 */
    CHECK(app_param_record_store(0x54455354U, &in, sizeof(in), 3) == 0);
    memset(&out, 0, sizeof(out));
    CHECK(app_param_record_load(0x54455354U, &out, sizeof(out), 3) == 0);
    CHECK(memcmp(&out, &in, sizeof(out)) == 0);
    /* 代次不符（本次烧录清除语义）：整条忽略 */
    memset(&out, 0x5A, sizeof(out));
    untouched = out;
    CHECK(app_param_record_load(0x54455354U, &out, sizeof(out), 4) != 0);
    CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
    /* 长度不符（旧schema记录）：整条忽略 */
    CHECK(app_param_record_load(0x54455354U, &out, sizeof(out) / 2, 3) != 0);
    CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
    /* 非法入参 */
    CHECK(app_param_record_load(0x54455354U, NULL, sizeof(out), 3) != 0);
    CHECK(app_param_record_load(0x54455354U, &out, APP_PARAM_DATA_MAX, 3) != 0);
    CHECK(app_param_record_store(0x54455354U, NULL, sizeof(in), 3) != 0);
}

/** @brief 未初始化单例直接store：不得写全零（懒初始化保证有效值） */
static void test_store_before_init(void) {
    mock_flash_reset();
    CHECK(app_software_params_store() == 0);
    app_software_params_t out;
    memset(&out, 0, sizeof(out));
    CHECK(app_software_params_load(&out) == 0);
    CHECK(out.control.speed_loop.kp == app_software_params_default()->control.speed_loop.kp);
    CHECK(out.control.limits.i_q_max_a == app_software_params_default()->control.limits.i_q_max_a);
    mock_flash_reset();
    CHECK(app_motor_params_store() == 0);
    app_motor_params_t motor;
    memset(&motor, 0, sizeof(motor));
    CHECK(app_motor_params_load(&motor) == 0);
    CHECK(motor.reducer_ratio == app_motor_params_default()->reducer_ratio);
    CHECK(motor.kt_nm_per_a == app_motor_params_default()->kt_nm_per_a);
}

/** @brief 参数域持久化：store→load 回读；代次 bump 后不叠加（出厂值/内存保持） */
static void test_params_persistence(void) {
    mock_flash_reset();
    uint32_t storage = app_software_params_default()->storage;
    CHECK(storage == APP_SW_STORAGE_KEEP);
    app_motor_params_load(app_motor_params_mutable());
    app_software_params_load(app_software_params_mutable());
    /* 在线修改并持久化 */
    app_motor_params_mutable()->reducer_ratio = 25.0f;
    app_software_params_mutable()->control.speed_loop.kp = 9.0f;
    CHECK(app_motor_params_store() == 0);
    CHECK(app_software_params_store() == 0);
    /* 模拟重启：先回落工厂，再 load 应恢复 flash 值 */
    *app_motor_params_mutable() = *app_motor_params_default();
    *app_software_params_mutable() = *app_software_params_default();
    CHECK(app_motor_params_load(app_motor_params_mutable()) == 0);
    CHECK(app_software_params_load(app_software_params_mutable()) == 0);
    CHECK(app_motor_params_current()->reducer_ratio == 25.0f);
    CHECK(app_software_params_current()->control.speed_loop.kp == 9.0f);
    /* 代次 bump（本次烧录清除）：记录被忽略，内存不被非法数据覆盖 */
    app_motor_params_mutable()->reducer_ratio = 1.0f;
    app_software_params_mutable()->control.speed_loop.kp = 42.0f;
    CHECK(app_motor_params_store() == 0);
    CHECK(app_software_params_store() == 0);
    CHECK(
        app_param_record_store(
            APP_PARAM_KEY_SOFTWARE_PARAMS, app_software_params_default(),
            sizeof(app_software_params_t), 2U)
        == 0); /* 异代次记录 */
    CHECK(app_software_params_load(app_software_params_mutable()) != 0);
    /* 代次不符 = 本次烧录清除语义：整体回落出厂值（非保留内存） */
    CHECK(
        app_software_params_current()->control.speed_loop.kp
        == app_software_params_default()->control.speed_loop.kp);
    CHECK(app_software_params_current()->storage == storage);
}

/** @brief P0-B observer/输出延迟字段 + 旧版短记录前缀迁移（不丢运动整定） */
static void test_observer_params_migration(void) {
    mock_flash_reset();
    const app_software_params_t* def = app_software_params_default();

    CHECK(def->speed_observer.enable == 1U);
    CHECK(def->speed_observer.bandwidth_rad_s == 500.0f);
    CHECK(def->speed_observer.max_speed_rad_s == 700.0f);
    CHECK(fabsf(def->current_loop_output_delay_s - 3.3333333e-5f) < 1e-9f);
    CHECK(def->motion.output_direction == 1.0f);
    CHECK(def->current_ff.r_en == 0U);
    CHECK(def->current_ff.wl_en == 0U);
    CHECK(def->current_ff.bemf_en == 0U);
    /* motion 电流硬限与矢量限幅同源（24.3A），避免两套限值不一致 */
    CHECK(def->control.limits.motion_iq_max_a == def->control.limits.i_q_max_a);

    size_t legacy_size = offsetof(app_software_params_t, speed_observer);
    CHECK(legacy_size > 0U && legacy_size < sizeof(app_software_params_t));
    app_software_params_t legacy = *def;
    legacy.control.speed_loop.kp = 7.25f;
    legacy.control.limits.motion_iq_max_a = 3.5f;
    CHECK(
        app_param_record_store(
            APP_PARAM_KEY_SOFTWARE_PARAMS, &legacy, legacy_size, 1U)
        == 0); /* 旧结构体长度，无新增字段 */
    app_software_params_t out;
    memset(&out, 0xA5, sizeof(out));
    CHECK(app_software_params_load(&out) == 0);
    CHECK(out.control.speed_loop.kp == 7.25f);
    CHECK(out.control.limits.motion_iq_max_a == 3.5f);
    /* 新增字段保持出厂默认：旧记录不覆盖它们 */
    CHECK(out.speed_observer.enable == def->speed_observer.enable);
    CHECK(out.speed_observer.bandwidth_rad_s == def->speed_observer.bandwidth_rad_s);
    CHECK(out.current_loop_output_delay_s == def->current_loop_output_delay_s);
    CHECK(out.motion.output_direction == def->motion.output_direction);
    CHECK(out.current_ff.r_en == def->current_ff.r_en);
    CHECK(out.current_ff.wl_en == def->current_ff.wl_en);
    CHECK(out.current_ff.bemf_en == def->current_ff.bemf_en);

    /* 新格式回存后，新增字段也应持久化 */
    out.speed_observer.enable = 0U;
    out.speed_observer.bandwidth_rad_s = 123.0f;
    out.current_loop_output_delay_s = 1.5e-5f;
    out.motion.output_direction = -1.0f;
    out.current_ff.r_en = 1U;
    out.current_ff.wl_en = 1U;
    out.current_ff.bemf_en = 1U;
    CHECK(
        app_param_record_store(
            APP_PARAM_KEY_SOFTWARE_PARAMS, &out, sizeof(out), 1U)
        == 0);
    app_software_params_t out2;
    memset(&out2, 0xA5, sizeof(out2));
    CHECK(app_software_params_load(&out2) == 0);
    CHECK(out2.speed_observer.enable == 0U);
    CHECK(out2.speed_observer.bandwidth_rad_s == 123.0f);
    CHECK(out2.current_loop_output_delay_s == 1.5e-5f);
    CHECK(out2.motion.output_direction == -1.0f);
    CHECK(out2.current_ff.r_en == 1U);
    CHECK(out2.current_ff.wl_en == 1U);
    CHECK(out2.current_ff.bemf_en == 1U);
    CHECK(out2.control.speed_loop.kp == 7.25f);
}

int main(void) {
    printf("Params host tests\n");
    test_param_record();
    test_store_before_init();
    test_observer_params_migration();
    test_params_persistence();
    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
