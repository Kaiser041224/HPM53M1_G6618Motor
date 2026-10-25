/**
 * @file app_current_identify.c
 * @brief Rs/Ld 与 PI 自整定编排（仅1kHz任务；不新增保护关机）
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app_current_identify.h"
#include "app_cold_ram.h"
#include "app_encoder.h"
#include "app_foc.h"
#include "app_foc_current.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_param.h"
#include "app_software_params.h"
#include "foc_math.h"
#include "intf_sys.h"
#include <math.h>
#include <string.h>

static app_current_identify_result_t s_current_identify_result;
static uint16_t s_current_identify_ticks;
static uint8_t s_current_identify_index;
static float s_current_identify_currents[5], s_current_identify_voltages[5];
static float s_current_identify_ld[2];
static bool s_current_identify_applied;
static bool s_ri_test_phase;
static uint32_t s_identify_pwm_hz, s_identify_pole_pairs;
/* 位移门限 [rad 电角]：0.5 rad≈2.9°机械（10 对极）。50:1 减速器带载时
 * cal encoder 实测回差 8.6~10.6°电角；原 0.15 rad 会在正常齿隙内误报。 */
#define APP_CURRENT_IDENTIFY_MOVEMENT_MAX_RAD (0.5f)
static float s_current_identify_origin_rotor;
static bool s_pi_baseline_armed;
static double s_phase_sum[3];
static double s_pi_baseline_sum, s_pi_baseline_variance_sum;
static const float s_current_identify_targets[] = {2, 3, 4, 3, 2};
static float s_current_identify_bias_voltage;
static uint16_t s_current_identify_blocks;
static uint8_t s_current_identify_repeats;
static uint16_t s_ld_target_blocks;
static double s_current_identify_sum, s_current_identify_q_sum, s_current_identify_voltage_sum;
static double s_current_identify_first_half_sum;
static float s_current_identify_voltage_base;
static id_rl_sample_t s_current_identify_average[APP_FOC_IDENTIFY_SAMPLES]
    APP_ATTR_AHB_SRAM; /**< 电流辨识平均缓冲（仅 cal current 使用） */
#define CURRENT_IDENTIFY_BLOCKS     20U
#define CURRENT_IDENTIFY_REPEATS    16U
#define CURRENT_IDENTIFY_LD_REPEATS 64U
/* step 未通过时自动降带宽重试：2kHz 在 30kHz/一拍输出延迟下相位裕度不足，
 * 实测阶跃超调会超过 20%。只降低 trial 带宽，不改用户配置；最多三次。 */
#define CURRENT_IDENTIFY_BW_RETRY_FACTOR (0.7f)
#define CURRENT_IDENTIFY_BW_RETRY_MAX    (3U)
#define CURRENT_IDENTIFY_BW_MIN_HZ       (200.0f)
/* 正常流程在 29~30s 附近，原 30s 超时会与正常完成竞争。 */
#define CURRENT_IDENTIFY_TIMEOUT_MS      (60000U)

/** @brief 复用固定缓冲累加重复响应，任务上下文执行 */
static void current_identify_average_clear(void) {
    memset(s_current_identify_average, 0, sizeof(s_current_identify_average));
    s_current_identify_repeats = 0;
    s_ld_target_blocks = CURRENT_IDENTIFY_LD_REPEATS;
}
/** @brief 发布有限恒压租约；到期回到零给定PI，不持续输出 */
static int current_identify_voltage(float voltage) {
    s_current_identify_bias_voltage = voltage;
    return app_foc_calib_voltage(voltage, app_hardware_params_current()->inverter.pwm_freq_hz * 2U);
}

/** @brief 持久化记录，参数口径等效Rs与Ld=Lq近似 */
typedef struct {
    uint32_t version, pwm_hz, pole_pairs;
    float rs, ld, kp, ki, bandwidth;
} current_identify_record_t;
_Static_assert(sizeof(current_identify_record_t)<=APP_PARAM_DATA_MAX,"FCUR record exceeds slot");

/** @brief FCUR 记录是否与当前硬件条件匹配且数值合法 */
static bool current_identify_record_valid(const current_identify_record_t* record) {
    if (record == NULL) {
        return false;
    }
    return (record->version == 1U)
           && (record->pwm_hz == app_hardware_params_current()->inverter.pwm_freq_hz)
           && (record->pole_pairs == app_motor_params_current()->pole_pairs)
           && foc_finite(record->rs) && (record->rs > 0.0f)
           && foc_finite(record->ld) && (record->ld > 0.0f)
           && foc_finite(record->kp) && (record->kp > 0.0f)
           && foc_finite(record->ki) && (record->ki > 0.0f)
           && foc_finite(record->bandwidth) && (record->bandwidth > 0.0f)
           && (record->pwm_hz != 0U) && (record->pole_pairs != 0U)
           && (record->bandwidth <= (float)record->pwm_hz / 15.0f);
}

/** @brief 完成/失败均释放校准端口并恢复临时参数 */
static void current_identify_finish(int reason) {
    s_current_identify_result.failure_stage = s_current_identify_result.state;
    app_foc_calib_end();
    s_current_identify_result.active = false;
    s_current_identify_result.failed = reason != 0;
    s_current_identify_result.done = reason == 0;
    s_current_identify_result.reason = reason;
    s_current_identify_result.state = APP_CURRENT_IDENTIFY_IDLE;
    s_current_identify_result.current_ref_a = 0;
}
/** @brief 发布状态与给定 */
static void current_identify_stage(app_current_identify_state_t state, float ref) {
    s_current_identify_result.state = state;
    s_current_identify_result.current_ref_a = ref;
    s_current_identify_ticks = 0;
    if (app_foc_calib_set_current(ref, 0)) {
        s_current_identify_result.detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
        current_identify_finish(2);
    }
}
int app_current_identify_start(void) {
    float offset, direction;
    float bandwidth = app_software_params_current()->control.current_loop.bandwidth_rad_s;
    uint32_t pwm_hz = app_hardware_params_current()->inverter.pwm_freq_hz;
    /* 试验上界fs/15：30kHz时允许2kHz（25kHz时约1.67kHz）。不是实测相位裕度保证。 */
    if (!foc_finite(bandwidth) || bandwidth <= 0 || pwm_hz == 0
        || bandwidth > (float)pwm_hz * (6.283185307f / 15.0f))
        return -1;
    app_encoder_rotor_snapshot_t rotor;
    if (s_current_identify_result.active || app_foc_get_state() != APP_FOC_STATE_READY
        || app_hardware_params_current()->inverter.pwm_freq_hz == 0)
        return -1;
    if (app_encoder_get_rotor_snapshot(&rotor) || !rotor.valid || !foc_finite(rotor.rad))
        return -1;
    if (app_foc_calib_begin())
        return -1;
    if (app_foc_calib_trial_r_ff(0U)) {
        app_foc_calib_end();
        return -1;
    }
    app_foc_get_encoder_offset(&offset, &direction);
    if (app_foc_calib_use_trial_offset(offset, direction)) {
        app_foc_calib_end();
        return -1;
    }
    s_current_identify_result = (app_current_identify_result_t){.active = true};
    s_ri_test_phase = false;
    s_current_identify_result.trial_bandwidth_hz = bandwidth / 6.283185307f;
    s_current_identify_applied = false;
    s_identify_pwm_hz=pwm_hz;
    s_identify_pole_pairs=app_motor_params_current()->pole_pairs;
    s_pi_baseline_armed = false;
    s_pi_baseline_sum = s_pi_baseline_variance_sum = 0;
    s_current_identify_index = 0;
    s_current_identify_voltage_base = app_motor_params_current()->rs_ohm * 2.0f;
    s_current_identify_origin_rotor = rotor.rad;
    float theta =
        foc_wrap_2pi(direction * rotor.rad * app_motor_params_current()->pole_pairs - offset);
    s_current_identify_result.excitation_theta_rad = theta;
    if (app_foc_calib_set_excitation(theta, 0, 0) || current_identify_voltage(0)) {
        current_identify_finish(2);
        return -1;
    }
    current_identify_stage(APP_CURRENT_IDENTIFY_ZERO_SETTLE, 0);
    return s_current_identify_result.active ? 0 : -1;
}
void app_current_identify_abort(void) {
    if (s_current_identify_result.active)
        current_identify_finish(1);
}
bool app_current_identify_is_active(void) { return s_current_identify_result.active; }
void app_current_identify_get_result(app_current_identify_result_t* out) {
    if (out)
        *out = s_current_identify_result;
}
/** @brief 冻结缓冲的平台质量与均值检查 */
static int current_identify_mean(const id_rl_sample_t* data, float* current, float* voltage) {
    app_current_identify_result_t* result = &s_current_identify_result;
    float sum = 0, volt = 0, low = data[0].i_d_a, high = low;
    result->capture_count = APP_FOC_IDENTIFY_SAMPLES;
    result->capture_invalid_count = 0;
    result->capture_q_peak_a = 0;
    result->capture_ref_a = result->current_ref_a;
    result->platform_index = s_current_identify_index;
    for (unsigned i = 0; i < APP_FOC_IDENTIFY_SAMPLES; ++i) {
        if (!data[i].valid || !foc_finite(data[i].i_d_a) || !foc_finite(data[i].i_q_a)
            || !foc_finite(data[i].v_d_previous_v)) {
            result->capture_invalid_count++;
            continue;
        }
        result->capture_q_peak_a = fmaxf(result->capture_q_peak_a, fabsf(data[i].i_q_a));
        sum += data[i].i_d_a;
        volt += data[i].v_d_previous_v;
        low = fminf(low, data[i].i_d_a);
        high = fmaxf(high, data[i].i_d_a);
    }
    *current = sum / APP_FOC_IDENTIFY_SAMPLES;
    *voltage = volt / APP_FOC_IDENTIFY_SAMPLES;
    result->capture_mean_a = *current;
    result->capture_voltage_v = *voltage;
    result->capture_ripple_a = high - low;
    if (result->capture_invalid_count)
        result->detail = APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID;
    else
        result->detail = APP_CURRENT_IDENTIFY_DETAIL_NONE;
    return result->detail == APP_CURRENT_IDENTIFY_DETAIL_NONE ? 0 : -1;
}
void app_current_identify_run_once(void) {
    app_current_identify_result_t* result = &s_current_identify_result;
    if (!result->active)
        return;
    result->elapsed_ms++;
    if (s_identify_pwm_hz!=app_hardware_params_current()->inverter.pwm_freq_hz
        || s_identify_pole_pairs!=app_motor_params_current()->pole_pairs) {
        result->detail=APP_CURRENT_IDENTIFY_DETAIL_STATE;
        current_identify_finish(2); return;
    }
    s_current_identify_ticks++;
    if (app_foc_get_state() != APP_FOC_STATE_CALIB
        || result->elapsed_ms > CURRENT_IDENTIFY_TIMEOUT_MS) {
        result->detail = app_foc_get_state() != APP_FOC_STATE_CALIB
                           ? APP_CURRENT_IDENTIFY_DETAIL_STATE
                           : APP_CURRENT_IDENTIFY_DETAIL_TIMEOUT;
        current_identify_finish(2);
        return;
    }
    app_foc_current_snapshot_t snapshot;
    uint32_t status = intf_sys_irq_save();
    app_foc_current_get_snapshot(&snapshot);
    intf_sys_irq_restore(status);
    app_encoder_rotor_snapshot_t rotor;
    int rotor_rc = app_encoder_get_rotor_snapshot(&rotor);
    result->theta_delta_rad =
        rotor_rc == 0 && rotor.valid
            ? remainderf(rotor.rad - s_current_identify_origin_rotor, 6.283185307f)
                  * app_motor_params_current()->pole_pairs
            : 0;
    /* 不可信数据/明显转动拒绝辨识，不执行关机。 */
    if (!snapshot.valid || !foc_finite(snapshot.theta_e_rad) || rotor_rc || !rotor.valid
        || !foc_finite(rotor.rad)
        || fabsf(result->theta_delta_rad) > APP_CURRENT_IDENTIFY_MOVEMENT_MAX_RAD) {
        result->detail = !snapshot.valid || !foc_finite(snapshot.theta_e_rad)
                           ? APP_CURRENT_IDENTIFY_DETAIL_SNAPSHOT
                           : APP_CURRENT_IDENTIFY_DETAIL_MOVEMENT;
        current_identify_finish(2);
        return;
    }
    float dt = 1.0f / app_hardware_params_current()->inverter.pwm_freq_hz;
    uint16_t count;
    status = intf_sys_irq_save();
    const id_rl_sample_t* data = app_foc_current_identify_data(&count);
    intf_sys_irq_restore(status);
    id_rl_fit_t fit;
    switch (result->state) {
    case APP_CURRENT_IDENTIFY_ZERO_SETTLE:
        if (s_current_identify_ticks < 100)
            break;
        if (app_foc_calib_capture(0, false)) {
            current_identify_finish(2);
            break;
        }
        s_current_identify_blocks = 0;
        s_current_identify_sum = s_current_identify_q_sum = 0;
        memset(s_phase_sum, 0, sizeof(s_phase_sum));
        result->state = APP_CURRENT_IDENTIFY_ZERO_CAPTURE;
        break;
    case APP_CURRENT_IDENTIFY_ZERO_CAPTURE:
        if (count != APP_FOC_IDENTIFY_SAMPLES)
            break;
        for (unsigned i = 0; i < count; ++i) {
            if (!data[i].valid || !foc_finite(data[i].i_d_a) || !foc_finite(data[i].i_q_a)) {
                result->detail = APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID;
                current_identify_finish(2);
                return;
            }
            s_current_identify_sum += data[i].i_d_a;
            s_current_identify_q_sum += data[i].i_q_a;
            s_phase_sum[0] += data[i].i_u_a;
            s_phase_sum[1] += data[i].i_v_a;
            s_phase_sum[2] += data[i].i_w_a;
        }
        if (++s_current_identify_blocks < 20) {
            if (app_foc_calib_capture(0, false))
                current_identify_finish(2);
            break;
        }
        result->zero_d_mean_a = (float)(s_current_identify_sum / (20U * APP_FOC_IDENTIFY_SAMPLES));
        result->zero_q_mean_a =
            (float)(s_current_identify_q_sum / (20U * APP_FOC_IDENTIFY_SAMPLES));
        for (unsigned i = 0; i < 3; ++i)
            result->zero_phase_a[i] = (float)(s_phase_sum[i] / (20U * APP_FOC_IDENTIFY_SAMPLES));
        /* 不把真实电流或逆变器误差自动扣作ADC零点。 */
        if (current_identify_voltage(s_current_identify_voltage_base)) {
            current_identify_finish(2);
            break;
        }
        current_identify_stage(APP_CURRENT_IDENTIFY_SETTLE, 2);
        break;
    case APP_CURRENT_IDENTIFY_SETTLE:
        /* 独立低带宽积分调压，参考ODrive ResistanceMeasurementControlLaw。
         * 仅校准任务使用均值，冻结后采集平台；不借用正式电流PI。 */
        if (s_current_identify_ticks % 5U == 0U && s_current_identify_ticks < 600) {
            float voltage =
                s_current_identify_bias_voltage
                + 0.005f
                      * (s_current_identify_targets[s_current_identify_index] - snapshot.i_d_avg_a);
            voltage = fminf(1.5f, fmaxf(0.0f, voltage));
            if (current_identify_voltage(voltage)) {
                current_identify_finish(2);
                break;
            }
        }
        if (s_current_identify_ticks < 600)
            break;
        if (app_foc_calib_capture(0, false)) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
            current_identify_finish(2);
            break;
        }
        result->state = APP_CURRENT_IDENTIFY_RS;
        s_current_identify_blocks = 0;
        s_current_identify_first_half_sum = 0;
        s_current_identify_sum = s_current_identify_q_sum = s_current_identify_voltage_sum = 0;
        memset(s_phase_sum, 0, sizeof(s_phase_sum));
        break;
    case APP_CURRENT_IDENTIFY_RS:
        if (count != APP_FOC_IDENTIFY_SAMPLES)
            break;
        if (current_identify_mean(
                data, &s_current_identify_currents[s_current_identify_index],
                &s_current_identify_voltages[s_current_identify_index])) {
            current_identify_finish(2);
            break;
        }
        float block_mean = s_current_identify_currents[s_current_identify_index];
        s_current_identify_sum += block_mean;
        s_current_identify_voltage_sum += s_current_identify_voltages[s_current_identify_index];
        for (unsigned i = 0; i < count; ++i) {
            s_current_identify_q_sum += data[i].i_q_a / count;
            s_phase_sum[0] += data[i].i_u_a;
            s_phase_sum[1] += data[i].i_v_a;
            s_phase_sum[2] += data[i].i_w_a;
        }
        if (s_current_identify_blocks < CURRENT_IDENTIFY_BLOCKS / 2U)
            s_current_identify_first_half_sum += block_mean;
        ++s_current_identify_blocks;
        result->capture_count = s_current_identify_blocks * count;
        result->capture_mean_a = (float)(s_current_identify_sum / s_current_identify_blocks);
        if (s_current_identify_blocks < CURRENT_IDENTIFY_BLOCKS) {
            if (app_foc_calib_capture(0, false))
                current_identify_finish(2);
            break;
        }
        float q_mean = (float)(s_current_identify_q_sum / s_current_identify_blocks);
        result->capture_q_mean_a = q_mean;
        for (unsigned i = 0; i < 3; ++i)
            result->driven_phase_a[i] =
                (float)(s_phase_sum[i] / (s_current_identify_blocks * count));
        /* q 轴零偏在同一固定坐标系、零电压阶段已测量；Rs 平台只应按
         * 相对增量判角度/相序错误，否则 0.17A 零偏会把 0.25A 阈值误触发。 */
        float q_zero = foc_finite(result->zero_q_mean_a) ? result->zero_q_mean_a : 0.0f;
        if (fabsf(q_mean - q_zero) > 0.25f) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_Q_CURRENT;
            current_identify_finish(2);
            break;
        }
        result->rs_window_drift_a =
            (float)((s_current_identify_sum - 2 * s_current_identify_first_half_sum)
                    / (CURRENT_IDENTIFY_BLOCKS / 2U));
        if (fabsf(result->rs_window_drift_a) > 0.15f) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_RIPPLE;
            current_identify_finish(2);
            break;
        }
        s_current_identify_currents[s_current_identify_index] = result->capture_mean_a;
        s_current_identify_voltages[s_current_identify_index] =
            (float)(s_current_identify_voltage_sum / s_current_identify_blocks);
        if (++s_current_identify_index < 5) {
            /* 电压递增至名义4A再递减；不是每拍闭环调压。 */
            static const float ratios[] = {1, 1.5f, 2, 1.5f, 1};
            /* 第一平台均值校正基础电压，平台间调节，不引入每拍PI。 */
            if (s_current_identify_index == 1 && result->capture_mean_a > 0.5f)
                s_current_identify_voltage_base *= 2.0f / result->capture_mean_a;
            if (current_identify_voltage(
                    s_current_identify_voltage_base * ratios[s_current_identify_index])) {
                current_identify_finish(2);
                break;
            }
            current_identify_stage(
                APP_CURRENT_IDENTIFY_SETTLE, s_current_identify_targets[s_current_identify_index]);
            break;
        }
        if (id_rl_resistance(s_current_identify_currents, s_current_identify_voltages, 5, &fit)) {
            current_identify_finish(3);
            break;
        }
        result->rs_ohm = fit.value;
        result->rs_residual_v = fit.residual;
        s_current_identify_index = 0;
        current_identify_average_clear();
        result->progress = 0.4f;
        if (current_identify_voltage(result->rs_ohm * 3 + fit.intercept)) {
            current_identify_finish(2);
            break;
        }
        current_identify_stage(APP_CURRENT_IDENTIFY_LD_SETTLE, 0);
        break;
    case APP_CURRENT_IDENTIFY_LD_SETTLE:
        /* 64块+恢复等待可能超过原2秒租约，必须显式续租，避免返回PI污染激励。 */
        if (s_current_identify_ticks == 1
            && current_identify_voltage(s_current_identify_bias_voltage)) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
            current_identify_finish(2);
            break;
        }
        if (s_current_identify_ticks < (s_current_identify_repeats ? 30 : 400))
            break;
        /* ODrive交替电压思想，适配3A偏置和4/8拍半周期；正常PI不参与。 */
        if (app_foc_calib_pulse(0.6f, s_current_identify_index ? 8U : 4U)) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
            current_identify_finish(2);
            break;
        }
        result->state = APP_CURRENT_IDENTIFY_LD;
        break;
    case APP_CURRENT_IDENTIFY_LD:
        if (count != APP_FOC_IDENTIFY_SAMPLES)
            break;
        for (unsigned i = 0; i < count; ++i) {
            if (!data[i].valid) {
                current_identify_finish(2);
                return;
            }
            float weight = 1.0f / (s_current_identify_repeats + 1U);
            s_current_identify_average[i].i_d_a +=
                weight * (data[i].i_d_a - s_current_identify_average[i].i_d_a);
            s_current_identify_average[i].i_q_a +=
                weight * (data[i].i_q_a - s_current_identify_average[i].i_q_a);
            s_current_identify_average[i].v_d_previous_v +=
                weight * (data[i].v_d_previous_v - s_current_identify_average[i].v_d_previous_v);
            s_current_identify_average[i].valid = true;
        }
        if (++s_current_identify_repeats < s_ld_target_blocks) {
            current_identify_stage(APP_CURRENT_IDENTIFY_LD_SETTLE, 0);
            break;
        }
        fit = (id_rl_fit_t){0};
        result->ld_fit_status =
            id_rl_pulse_inductance(s_current_identify_average, count, dt, result->rs_ohm, &fit);
        if (result->ld_fit_status) {
            result->ld_rejected_h = fit.value;
            result->ld_rejected_residual_a = fit.residual;
            if (result->ld_fit_status == -4 && s_ld_target_blocks == CURRENT_IDENTIFY_LD_REPEATS) {
                s_ld_target_blocks = 2U * CURRENT_IDENTIFY_LD_REPEATS;
                result->ld_extra_batches++;
                current_identify_stage(APP_CURRENT_IDENTIFY_LD_SETTLE, 0);
                break;
            }
            current_identify_finish(3);
            break;
        }
        s_current_identify_ld[s_current_identify_index] = fit.value;
        if (s_current_identify_index == 0)
            result->ld_half4_h = fit.value;
        else
            result->ld_half8_h = fit.value;
        result->ld_residual_a = fmaxf(result->ld_residual_a, fit.residual);
        if (++s_current_identify_index < 2) {
            current_identify_average_clear();
            if (current_identify_voltage(s_current_identify_bias_voltage)) {
                current_identify_finish(2);
                break;
            }
            current_identify_stage(APP_CURRENT_IDENTIFY_LD_SETTLE, 0);
            break;
        }
        result->ld_h = (s_current_identify_ld[0] + s_current_identify_ld[1]) * 0.5f;
        result->ld_repeat_error =
            fabsf(s_current_identify_ld[0] - s_current_identify_ld[1]) / result->ld_h;
        if (result->ld_repeat_error > 0.2f) {
            current_identify_finish(3);
            break;
        }
        s_current_identify_index = 0;
        current_identify_average_clear();
        (void)app_foc_calib_voltage(-1, 0);
        /* 仍在同一固定坐标系验证电流调节器。
         * 编码器跟随角下id不提供位置恢复，不能配固定角位移判据。 */
        result->progress = 0.65f;
        current_identify_stage(APP_CURRENT_IDENTIFY_PI_SETTLE, 3);
        break;
    case APP_CURRENT_IDENTIFY_PI_SETTLE:
        if (s_current_identify_ticks == 1 && s_current_identify_repeats == 0) {
            float kp, ki;
            if (id_rl_pi(result->rs_ohm, result->ld_h, result->trial_bandwidth_hz, &kp, &ki)
                || app_foc_calib_trial_pi(kp, ki)) {
                current_identify_finish(3);
                break;
            }
            result->trial_kp = kp;
            result->trial_ki = ki;
        }
        if (s_current_identify_ticks < 400)
            break;
        if (!s_pi_baseline_armed) {
            if (app_foc_calib_capture(0, false)) {
                current_identify_finish(2);
                break;
            }
            s_pi_baseline_armed = true;
            break;
        }
        if (count != APP_FOC_IDENTIFY_SAMPLES)
            break;
        double sum = 0, square = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (!data[i].valid || !foc_finite(data[i].i_d_a)) {
                result->detail = APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID;
                current_identify_finish(2);
                return;
            }
            sum += data[i].i_d_a;
            square += (double)data[i].i_d_a * data[i].i_d_a;
        }
        double mean = sum / count;
        s_pi_baseline_sum += mean;
        s_pi_baseline_variance_sum += fmax(0, square / count - mean * mean);
        s_pi_baseline_armed = false;
        /* 同一临界区提交阶跃与捕获，避免ISR在两次发布之间运行。 */
        status = intf_sys_irq_save();
        int rc = app_foc_calib_set_current(3.5f, 0);
        if (!rc)
            rc = app_foc_calib_capture(0, false);
        intf_sys_irq_restore(status);
        if (rc) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
            current_identify_finish(2);
            break;
        }
        result->current_ref_a = 3.5f;
        result->state = APP_CURRENT_IDENTIFY_PI;
        break;
    case APP_CURRENT_IDENTIFY_PI:
        if (count != APP_FOC_IDENTIFY_SAMPLES)
            break;
        id_rl_step_result_t step;
        for (unsigned i = 0; i < count; ++i) {
            if (!data[i].valid) {
                result->detail = APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID;
                current_identify_finish(2);
                return;
            }
            s_current_identify_average[i].i_d_a += data[i].i_d_a / CURRENT_IDENTIFY_REPEATS;
            s_current_identify_average[i].i_q_a += data[i].i_q_a / CURRENT_IDENTIFY_REPEATS;
            s_current_identify_average[i].valid = true;
        }
        if (++s_current_identify_repeats < CURRENT_IDENTIFY_REPEATS) {
            current_identify_stage(APP_CURRENT_IDENTIFY_PI_SETTLE, 3);
            break;
        }
        result->pi_baseline_a = (float)(s_pi_baseline_sum / CURRENT_IDENTIFY_REPEATS);
        result->pi_baseline_rms_a =
            (float)sqrt(s_pi_baseline_variance_sum / CURRENT_IDENTIFY_REPEATS);
        if (id_rl_step(s_current_identify_average, count, dt, result->pi_baseline_a, 3.5f, &step)) {
            result->detail = APP_CURRENT_IDENTIFY_DETAIL_CAPTURE_INVALID;
            current_identify_finish(2);
            break;
        }
        result->trial_step = step;
        /* 2kHz 等较高带宽在采样/输出延迟下可能超调；step 未通过时按固定比例
         * 降到下一档重试。只重试“超调型”失败，跟踪误差/纹波失败不降带宽。
         * 不修改用户的带宽配置，最终结果显式报告实际 trial 带宽。 */
        if (!step.passed
            && ((step.overshoot > 0.2f) || (step.overshoot_raw > ID_RL_OVERSHOOT_RAW_MAX))
            && (result->bandwidth_retries < CURRENT_IDENTIFY_BW_RETRY_MAX)) {
            float next_hz = result->trial_bandwidth_hz * CURRENT_IDENTIFY_BW_RETRY_FACTOR;

            if (next_hz >= CURRENT_IDENTIFY_BW_MIN_HZ) {
                result->trial_bandwidth_hz = next_hz;
                result->bandwidth_retries++;
                current_identify_average_clear();
                s_pi_baseline_sum = 0;
                s_pi_baseline_variance_sum = 0;
                s_pi_baseline_armed = false;
                current_identify_stage(APP_CURRENT_IDENTIFY_PI_SETTLE, 3);
                break;
            }
        }
        /* 先记录 R*I 关闭的最终基线；通过后在同一 d 轴小电流阶跃上
         * 强制开启 R*I 再跑一轮，评估其对动态指标的实际影响。 */
        if (step.passed && !s_ri_test_phase) {
            result->r_ff_step_off = step;
            if (app_foc_calib_trial_r_ff(1U)) {
                result->detail = APP_CURRENT_IDENTIFY_DETAIL_PORT;
                current_identify_finish(2);
                break;
            }
            s_ri_test_phase = true;
            result->r_ff_tested = false;
            current_identify_average_clear();
            s_pi_baseline_sum = 0;
            s_pi_baseline_variance_sum = 0;
            s_pi_baseline_armed = false;
            current_identify_stage(APP_CURRENT_IDENTIFY_PI_SETTLE, 3);
            break;
        }
        if (s_ri_test_phase) {
            result->r_ff_step_on = step;
            result->r_ff_tested = true;
            result->r_ff_recommended =
                step.passed
                && (step.rise_s <= (result->r_ff_step_off.rise_s * 1.15f + 1.0e-4f))
                && (step.overshoot <= result->r_ff_step_off.overshoot + 0.03f)
                && (step.error_a <= result->r_ff_step_off.error_a + 0.02f)
                && (step.ripple_rms_a
                    <= (result->r_ff_step_off.ripple_rms_a * 1.20f + 0.01f));
            s_ri_test_phase = false;
            (void)app_foc_calib_trial_r_ff(0U);
        }
        result->step = (result->r_ff_tested) ? result->r_ff_step_off : step;
        result->bandwidth_hz = result->trial_bandwidth_hz;
        result->kp=result->trial_kp; /* 已经过有限值检查并实际试用的增益 */
        result->ki=result->trial_ki;
        result->progress = 1;
        current_identify_finish(0);
        break;
    default: current_identify_finish(2); break;
    }
}
int app_current_identify_apply(void) {
    if (s_current_identify_result.active)
        return -3;
    if (app_foc_get_state() != APP_FOC_STATE_OFF)
        return -1;
    if (!s_current_identify_result.done)
        return -2;
    if (!s_current_identify_result.step.passed)
        return -5; /* 动态 step 未通过：不应用模型/增益，也不允许后续 save */
    if (s_identify_pwm_hz!=app_hardware_params_current()->inverter.pwm_freq_hz
        || s_identify_pole_pairs!=app_motor_params_current()->pole_pairs) return -4;
    app_motor_params_t* motor = app_motor_params_mutable();
    app_software_params_t* software = app_software_params_mutable();
    motor->rs_ohm = s_current_identify_result.rs_ohm;
    motor->ls_h = s_current_identify_result.ld_h;
    software->control.current_loop.kp = s_current_identify_result.kp;
    software->control.current_loop.ki = s_current_identify_result.ki;
    software->control.current_loop.bandwidth_rad_s =
        s_current_identify_result.bandwidth_hz * 6.283185307f;
    app_foc_current_init(); /* 同步解耦模型的电感 */
    s_current_identify_applied = true;
    return 0;
}
int app_current_identify_save(void) {
    if ((app_foc_get_state() != APP_FOC_STATE_OFF) || app_foc_stop_pending()
        || !s_current_identify_applied || !s_current_identify_result.done)
        return -1;
    const app_motor_params_t* motor = app_motor_params_current();
    const app_software_params_t* software = app_software_params_current();
    if (s_identify_pwm_hz!=app_hardware_params_current()->inverter.pwm_freq_hz
        || s_identify_pole_pairs!=motor->pole_pairs
        || motor->rs_ohm!=s_current_identify_result.rs_ohm
        || motor->ls_h!=s_current_identify_result.ld_h
        || software->control.current_loop.kp!=s_current_identify_result.kp
        || software->control.current_loop.ki!=s_current_identify_result.ki
        || software->control.current_loop.bandwidth_rad_s!=s_current_identify_result.bandwidth_hz*6.283185307f)
        return -1; /* 不保存被后续param编辑的未验证参数 */
    current_identify_record_t record = {
        1,
        app_hardware_params_current()->inverter.pwm_freq_hz,
        motor->pole_pairs,
        motor->rs_ohm,
        motor->ls_h,
        software->control.current_loop.kp,
        software->control.current_loop.ki,
        software->control.current_loop.bandwidth_rad_s / 6.283185307f};
    return app_param_store(APP_PARAM_KEY_FOC_CURRENT, &record, sizeof(record));
}
int app_current_identify_get_saved(app_current_identify_saved_t* out) {
    current_identify_record_t record;

    if (out == NULL) {
        return -1;
    }
    *out = (app_current_identify_saved_t){0};
    if (!app_param_is_ready()) {
        return -1;
    }
    if (app_param_load(APP_PARAM_KEY_FOC_CURRENT, &record, sizeof(record)) != 0) {
        return 1;
    }
    out->present = true;
    out->valid = current_identify_record_valid(&record);
    out->rs_ohm = record.rs;
    out->ld_h = record.ld;
    out->kp = record.kp;
    out->ki = record.ki;
    out->bandwidth_hz = record.bandwidth;
    return 0;
}

int app_current_identify_restore(void) {
    if (app_foc_get_state()!=APP_FOC_STATE_OFF || s_current_identify_result.active) return -1;
    current_identify_record_t record;
    if (!app_param_is_ready() || app_param_load(APP_PARAM_KEY_FOC_CURRENT, &record, sizeof(record)))
        return 1;
    if (!current_identify_record_valid(&record))
        return -1;
    app_motor_params_mutable()->rs_ohm = record.rs;
    app_motor_params_mutable()->ls_h = record.ld;
    app_software_params_mutable()->control.current_loop.kp = record.kp;
    app_software_params_mutable()->control.current_loop.ki = record.ki;
    app_software_params_mutable()->control.current_loop.bandwidth_rad_s =
        record.bandwidth * 6.283185307f;
    return 0;
}

int app_current_identify_clear(void) {
    if (app_foc_get_state() != APP_FOC_STATE_OFF || s_current_identify_result.active) {
        return -1;
    }
    return app_param_erase(APP_PARAM_KEY_FOC_CURRENT);
}
