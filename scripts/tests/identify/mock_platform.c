/**
 * @file    mock_platform.c
 * @brief   宿主测试替身：app_motor_identify 依赖的 FOC 校准端口 / 编码器 / 参数 / 存储 / 时钟
 * @author  Kaiser
 *
 * 转子物理替身（理想电流环）：
 *   - 转子真实电角度 β；机械角 θm = (β + off_true) / (p·dir_true)
 *   - 电流矢量电角度 α：强制角模式 = θcmd + atan2(iq, id)；
 *                      编码器模式 = (p·dir_est·θm − off_est) + atan2(iq, id)
 *   - 每拍（1ms）β += K·sin(α − β)（|I| > 0 时）：强制角下转子跟随，编码器模式下
 *     零点正确时 i_d 锁定静止、i_q 探针沿正方向转动
 *
 * 仅用于宿主机自测，不参与目标固件。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "foc_math.h"

#include <math.h>
#include <string.h>

#define MOCK_POLE_PAIRS    (10U)
#define MOCK_PLANT_GAIN    (0.5f) /**< 每拍跟随增益 */
#define MOCK_CYCLES_PER_MS (480000U)

/* ---- 可观测/可注入状态（测试读写） ---- */
app_foc_state_t g_mock_foc_state;
int g_mock_calib_begin_count;
int g_mock_calib_end_count;
int g_mock_apply_count;
float g_mock_applied_offset;
float g_mock_applied_dir;
float g_mock_off_true;
float g_mock_dir_true;
float g_mock_current_scale; /**< 实测电流 = 给定 × scale（过流注入） */
bool g_mock_probe_stuck;    /**< 编码器模式下转子卡死（验证失败注入） */
float g_mock_probe_min_a;   /**< q 探针最小起动力 [A]（自适应升流测试） */
bool g_mock_snap_invalid;   /**< 电流快照 valid=false（电流环保护路径注入） */
uint32_t g_mock_enc_errors;
uint32_t g_mock_enc_jumps;

static uint32_t s_cycle;
static bool s_forced;
static float s_theta_cmd;
static float s_id;
static float s_iq;
static float s_est_off;     /**< 编码器模式使用的零点（试用或运行值） */
static float s_est_dir;
static float s_run_off;     /**< 运行期已应用零点 */
static float s_run_dir;
static float s_beta;        /**< 转子真实电角度 */
static uint32_t s_seq;
static app_motor_params_t s_motor;

/* 参数存储替身（单记录） */
static uint8_t s_store_buf[APP_PARAM_DATA_MAX];
static size_t s_store_len;
static uint32_t s_store_key;
static bool s_store_valid;
int g_mock_store_count;

static float mock_theta_m(void) {
    return foc_wrap_2pi((s_beta + g_mock_off_true) / ((float)MOCK_POLE_PAIRS * g_mock_dir_true));
}

void mock_reset(float off_true, float dir_true) {
    g_mock_foc_state = APP_FOC_STATE_READY;
    g_mock_calib_begin_count = 0;
    g_mock_calib_end_count = 0;
    g_mock_apply_count = 0;
    g_mock_applied_offset = 0.0f;
    g_mock_applied_dir = 0.0f;
    g_mock_off_true = off_true;
    g_mock_dir_true = dir_true;
    g_mock_current_scale = 1.0f;
    g_mock_probe_stuck = false;
    g_mock_probe_min_a = 0.0f;
    g_mock_snap_invalid = false;
    g_mock_enc_errors = 0U;
    g_mock_enc_jumps = 0U;
    s_cycle = 0U;
    s_forced = false;
    s_theta_cmd = 0.0f;
    s_id = 0.0f;
    s_iq = 0.0f;
    s_run_off = 0.0f;
    s_run_dir = 1.0f;
    s_est_off = s_run_off;
    s_est_dir = s_run_dir;
    s_beta = 0.3f; /* 任意初始位置 */
    s_seq = 0U;
    memset(&s_motor, 0, sizeof(s_motor));
    s_motor.pole_pairs = MOCK_POLE_PAIRS;
    s_motor.encoder.electrical_offset_rad = 0.0f;
    s_motor.encoder.direction = 1.0f;
    s_motor.encoder.electrical_offset_valid = 0U;
}

void mock_store_reset(void) {
    memset(s_store_buf, 0, sizeof(s_store_buf));
    s_store_len = 0U;
    s_store_key = 0U;
    s_store_valid = false;
    g_mock_store_count = 0;
}

/**
 * @brief 推进 1ms：转子动力学 + 编码器序号 + 时钟
 */
void mock_plant_step(void) {
    float mag = sqrtf(s_id * s_id + s_iq * s_iq);

    if (mag > 0.0f) {
        float alpha;

        if (s_forced) {
            alpha = s_theta_cmd + atan2f(s_iq, s_id);
        } else {
            float theta_e_est = (float)MOCK_POLE_PAIRS * s_est_dir * mock_theta_m() - s_est_off;

            alpha = theta_e_est + atan2f(s_iq, s_id);
        }
        bool probe_too_weak =
            !s_forced && (fabsf(s_id) < 0.1f) && (fabsf(s_iq) < g_mock_probe_min_a);
        if (!(g_mock_probe_stuck && !s_forced) && !probe_too_weak) {
            s_beta += MOCK_PLANT_GAIN * sinf(foc_wrap_pm_pi(alpha - s_beta));
        }
    }
    s_seq++;
    s_cycle += MOCK_CYCLES_PER_MS;
}

/* ---- FOC 校准端口替身 ---- */
app_foc_state_t app_foc_get_state(void) { return g_mock_foc_state; }
bool app_foc_stop_pending(void) { return false; }

int app_foc_calib_begin(void) {
    if ((g_mock_foc_state != APP_FOC_STATE_READY) && (g_mock_foc_state != APP_FOC_STATE_RUN)) {
        return -1;
    }
    g_mock_calib_begin_count++;
    g_mock_foc_state = APP_FOC_STATE_CALIB;
    s_forced = true;
    s_theta_cmd = 0.0f;
    s_id = 0.0f;
    s_iq = 0.0f;
    return 0;
}

void app_foc_calib_end(void) {
    if (g_mock_foc_state == APP_FOC_STATE_CALIB) {
        g_mock_calib_end_count++;
        g_mock_foc_state = APP_FOC_STATE_READY;
    }
    s_forced = false;
    s_est_off = s_run_off; /* 恢复运行值（试用零点作废） */
    s_est_dir = s_run_dir;
    s_id = 0.0f;
    s_iq = 0.0f;
}

int app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref) {
    if (g_mock_foc_state != APP_FOC_STATE_CALIB) {
        return -1;
    }
    s_forced = true;
    s_theta_cmd = theta_e_rad;
    s_id = i_d_ref;
    s_iq = i_q_ref;
    return 0;
}

int app_foc_calib_use_trial_offset(float offset_rad, float direction) {
    if (g_mock_foc_state != APP_FOC_STATE_CALIB) {
        return -1;
    }
    s_forced = false;
    s_est_off = offset_rad;
    s_est_dir = direction;
    return 0;
}

int app_foc_calib_set_current(float i_d_ref, float i_q_ref) {
    if (g_mock_foc_state != APP_FOC_STATE_CALIB) {
        return -1;
    }
    s_id = i_d_ref;
    s_iq = i_q_ref;
    return 0;
}

int app_foc_apply_encoder_offset(float offset_rad, float direction) {
    if (g_mock_foc_state == APP_FOC_STATE_CALIB) {
        return -1;
    }
    g_mock_apply_count++;
    g_mock_applied_offset = offset_rad;
    g_mock_applied_dir = direction;
    s_run_off = offset_rad;
    s_run_dir = direction;
    s_est_off = offset_rad;
    s_est_dir = direction;
    return 0;
}

void app_foc_get_snapshot(app_foc_current_snapshot_t* out) {
    memset(out, 0, sizeof(*out));
    out->theta_e_rad =
        s_forced ? foc_wrap_2pi(s_theta_cmd)
                 : foc_wrap_2pi((float)MOCK_POLE_PAIRS * s_est_dir * mock_theta_m() - s_est_off);
    out->i_d_a = s_id * g_mock_current_scale;
    out->i_q_a = s_iq * g_mock_current_scale;
    out->v_bus_v = 24.0f;
    out->valid = !g_mock_snap_invalid;
}

/* ---- 编码器替身 ---- */
int app_encoder_get_rotor_snapshot(app_encoder_rotor_snapshot_t* out) {
    memset(out, 0, sizeof(*out));
    out->rad = mock_theta_m();
    out->raw = (uint16_t)(out->rad * (65536.0f / FOC_TWO_PI_F));
    out->seq = s_seq;
    out->timestamp_cycles = s_cycle;
    out->valid = true;
    return 0;
}

uint32_t app_encoder_get_error_count(app_encoder_id_t id) {
    (void)id;
    return g_mock_enc_errors;
}

uint32_t app_encoder_get_rotor_jump_count(void) { return g_mock_enc_jumps; }

/* ---- 故障 / 参数 / 时钟 ---- */
app_fault_state_t app_fault_get_state(void) { return APP_FAULT_STATE_NORMAL; }

const app_motor_params_t* app_motor_params_current(void) { return &s_motor; }
app_motor_params_t* app_motor_params_mutable(void) { return &s_motor; }

uint32_t intf_clock_get_cycle(void) { return s_cycle; }
uint32_t intf_clock_get_cpu_freq(void) { return MOCK_CYCLES_PER_MS * 1000U; }
void intf_clock_delay_ms(uint32_t ms) { (void)ms; }

/* ---- 参数存储替身（内存单记录） ---- */
bool app_param_is_ready(void) { return true; }

int app_param_load(uint32_t key, void* buf, size_t len) {
    if (!s_store_valid || (key != s_store_key) || (len != s_store_len)) {
        return -1;
    }
    memcpy(buf, s_store_buf, len);
    return 0;
}

int app_param_store(uint32_t key, const void* buf, size_t len) {
    if (len > sizeof(s_store_buf)) {
        return -1;
    }
    memcpy(s_store_buf, buf, len);
    s_store_len = len;
    s_store_key = key;
    s_store_valid = true;
    g_mock_store_count++;
    return 0;
}

int app_param_erase(uint32_t key) {
    if (s_store_valid && (key == s_store_key)) {
        s_store_valid = false;
        s_store_len = 0;
    }
    return 0;
}
