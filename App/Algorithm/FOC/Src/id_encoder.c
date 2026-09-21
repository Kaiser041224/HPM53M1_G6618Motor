/**
 * @file    id_encoder.c
 * @brief   电角度辨识实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "id_encoder.h"

#define ID_ENCODER_TS_FALLBACK (1.0f / 25000.0f) /**< 非法 dt 时的兜底步长 [s] */

/**
 * @brief 初始化
 */
static int id_encoder_init(id_encoder_t* self, const id_encoder_cfg_t* cfg) {
    if ((self == NULL) || (cfg == NULL)) {
        return -1;
    }
    if ((cfg->pole_pairs == 0U) || (cfg->sweep_steps < 8U) || (cfg->sweep_step_ms <= 0.0f)
        || (cfg->i_cal_a <= 0.0f) || (cfg->lockin_ms <= 0.0f) || (cfg->dir_ms <= 0.0f)
        || (cfg->timeout_ms <= 0.0f)) {
        return -1;
    }
    self->_cfg = *cfg;
    self->_inited = true;
    return 0;
}

/**
 * @brief 复位
 */
static void id_encoder_reset(id_encoder_t* self) {
    if (self == NULL) {
        return;
    }
    self->_phase = ID_ENCODER_PHASE_LOCK_IN;
    self->_t_ms = 0.0f;
    self->_elapsed_ms = 0.0f;
    self->_theta_cmd = 0.0f;
    self->_step_idx = 0U;
    self->_s_sum = 0.0f;
    self->_c_sum = 0.0f;
    self->_acc_n = 0U;
    self->_theta_m_start = 0.0f;
    self->_mech_travel = 0.0f;
    self->_theta_m_prev = 0.0f;
    self->_theta_m_prev_valid = false;
    self->_offset_rad = 0.0f;
    self->_direction = 1.0f;
    self->_quality = 0.0f;
    self->_mech_ratio_err = 0.0f;
}

/**
 * @brief 扫描阶段单步（FWD/REV 共用）
 * @return true = 该扫描段结束（进入下一阶段）
 */
static bool id_encoder_sweep_step(id_encoder_t* self, const id_encoder_in_t* in, bool fwd) {
    uint16_t steps = self->_cfg.sweep_steps;
    float frac;

    /* 目标角按步索引分档：FWD 0→2π；REV 2π→0 */
    frac = (float)self->_step_idx / (float)steps;
    self->_theta_cmd = fwd ? (FOC_TWO_PI_F * frac) : (FOC_TWO_PI_F * (1.0f - frac));

    /* 机械位移累计（wrap-safe；仅正向扫描累计，用于极对数校验）。
     * 仅在首个采样点之后累计：DIR 段结束时强制角由 dir_step_rad 回落至 0，
     * 转子随之回退的瞬态不属于正向扫描行程，须排除。 */
    if (fwd && self->_theta_m_prev_valid && (self->_acc_n > 0U)) {
        self->_mech_travel += foc_wrap_pm_pi(in->theta_m_raw_rad - self->_theta_m_prev);
    }
    self->_theta_m_prev = in->theta_m_raw_rad;
    self->_theta_m_prev_valid = true;

    /* 每步驻留结束采样一次（转子已稳定） */
    if (self->_t_ms >= self->_cfg.sweep_step_ms) {
        float p = (float)self->_cfg.pole_pairs;
        float delta = foc_wrap_pm_pi(self->_theta_cmd - p * self->_direction * in->theta_m_raw_rad);

        self->_s_sum += sinf(delta);
        self->_c_sum += cosf(delta);
        self->_acc_n++;

        self->_t_ms = 0.0f;
        self->_step_idx++;
    }

    return (self->_step_idx >= steps);
}

/**
 * @brief 单步
 */
static void id_encoder_step(id_encoder_t* self, const id_encoder_in_t* in,
                            id_encoder_out_t* out) {
    float dt_ms;

    if ((self == NULL) || (in == NULL) || (out == NULL) || !self->_inited) {
        return;
    }

    dt_ms = (in->dt_s > 0.0f) ? (in->dt_s * 1000.0f)
                              : (ID_ENCODER_TS_FALLBACK * 1000.0f);
    self->_t_ms += dt_ms;
    self->_elapsed_ms += dt_ms;

    /* 默认输出（激励请求 + 当前结果） */
    out->theta_e_cmd = self->_theta_cmd;
    out->i_d_ref = self->_cfg.i_cal_a;
    out->i_q_ref = 0.0f;
    out->phase = self->_phase;
    out->offset_rad = self->_offset_rad;
    out->direction = self->_direction;
    out->quality = self->_quality;
    out->mech_ratio_err = self->_mech_ratio_err;
    out->done = false;
    out->failed = false;

    /* 总超时 */
    if ((self->_phase != ID_ENCODER_PHASE_DONE) && (self->_phase != ID_ENCODER_PHASE_FAILED)
        && (self->_elapsed_ms > self->_cfg.timeout_ms)) {
        self->_phase = ID_ENCODER_PHASE_FAILED;
        out->phase = self->_phase;
        out->failed = true;
        return;
    }

    switch (self->_phase) {
    case ID_ENCODER_PHASE_LOCK_IN:
        self->_theta_cmd = 0.0f;
        if (self->_t_ms >= self->_cfg.lockin_ms) {
            self->_phase = ID_ENCODER_PHASE_DIR;
            self->_t_ms = 0.0f;
            self->_theta_m_start = in->theta_m_raw_rad;
            self->_theta_cmd = self->_cfg.dir_step_rad;
        }
        break;

    case ID_ENCODER_PHASE_DIR:
        if (self->_t_ms >= self->_cfg.dir_ms) {
            float delta = foc_wrap_pm_pi(in->theta_m_raw_rad - self->_theta_m_start);

            self->_direction = (delta >= 0.0f) ? 1.0f : -1.0f;
            self->_phase = ID_ENCODER_PHASE_SWEEP_FWD;
            self->_t_ms = 0.0f;
            self->_step_idx = 0U;
            self->_s_sum = 0.0f;
            self->_c_sum = 0.0f;
            self->_acc_n = 0U;
            self->_mech_travel = 0.0f;
            self->_theta_m_prev = in->theta_m_raw_rad;
            self->_theta_m_prev_valid = true;
            self->_theta_cmd = 0.0f;
        }
        break;

    case ID_ENCODER_PHASE_SWEEP_FWD:
        if (id_encoder_sweep_step(self, in, true)) {
            self->_phase = ID_ENCODER_PHASE_SWEEP_REV;
            self->_t_ms = 0.0f;
            self->_step_idx = 0U;
            self->_theta_cmd = FOC_TWO_PI_F;
        }
        break;

    case ID_ENCODER_PHASE_SWEEP_REV:
        if (id_encoder_sweep_step(self, in, false)) {
            self->_phase = ID_ENCODER_PHASE_COMPUTE;
            self->_t_ms = 0.0f;
        }
        break;

    case ID_ENCODER_PHASE_COMPUTE:
        if (self->_acc_n > 0U) {
            float p = (float)self->_cfg.pole_pairs;
            float expected_travel = FOC_TWO_PI_F / p; /* 一个电周期对应机械位移 */

            self->_offset_rad = foc_wrap_2pi(-atan2f(self->_s_sum, self->_c_sum));
            self->_quality =
                sqrtf(self->_s_sum * self->_s_sum + self->_c_sum * self->_c_sum)
                / (float)self->_acc_n;
            self->_mech_ratio_err =
                (expected_travel > 0.0f)
                    ? ((fabsf(self->_mech_travel) - expected_travel) / expected_travel)
                    : 0.0f;

            if (self->_quality < self->_cfg.quality_min) {
                self->_phase = ID_ENCODER_PHASE_FAILED;
            } else if ((self->_mech_ratio_err > self->_cfg.ratio_tol)
                       || (self->_mech_ratio_err < -self->_cfg.ratio_tol)) {
                self->_phase = ID_ENCODER_PHASE_FAILED;
            } else {
                self->_phase = ID_ENCODER_PHASE_DONE;
            }
        } else {
            self->_phase = ID_ENCODER_PHASE_FAILED;
        }
        break;

    case ID_ENCODER_PHASE_DONE:
    case ID_ENCODER_PHASE_FAILED:
    default:
        break;
    }

    /* 结果与状态回填 */
    out->theta_e_cmd = self->_theta_cmd;
    out->phase = self->_phase;
    out->offset_rad = self->_offset_rad;
    out->direction = self->_direction;
    out->quality = self->_quality;
    out->mech_ratio_err = self->_mech_ratio_err;
    out->progress = (self->_phase == ID_ENCODER_PHASE_DONE) ? 1.0f
                    : (self->_phase == ID_ENCODER_PHASE_SWEEP_FWD)
                        ? (0.25f + 0.25f * ((float)self->_step_idx / (float)self->_cfg.sweep_steps))
                    : (self->_phase == ID_ENCODER_PHASE_SWEEP_REV)
                        ? (0.5f + 0.25f * ((float)self->_step_idx / (float)self->_cfg.sweep_steps))
                    : (self->_phase == ID_ENCODER_PHASE_COMPUTE) ? 0.95f
                    : 0.0f;
    out->done = (self->_phase == ID_ENCODER_PHASE_DONE);
    out->failed = (self->_phase == ID_ENCODER_PHASE_FAILED);
}

/**
 * @brief 构造
 */
void id_encoder_ctor(id_encoder_t* self) {
    if (self == NULL) {
        return;
    }
    self->init = id_encoder_init;
    self->step = id_encoder_step;
    self->reset = id_encoder_reset;
    self->_phase = ID_ENCODER_PHASE_IDLE;
    self->_t_ms = 0.0f;
    self->_elapsed_ms = 0.0f;
    self->_theta_cmd = 0.0f;
    self->_step_idx = 0U;
    self->_s_sum = 0.0f;
    self->_c_sum = 0.0f;
    self->_acc_n = 0U;
    self->_theta_m_start = 0.0f;
    self->_mech_travel = 0.0f;
    self->_theta_m_prev = 0.0f;
    self->_theta_m_prev_valid = false;
    self->_offset_rad = 0.0f;
    self->_direction = 1.0f;
    self->_quality = 0.0f;
    self->_mech_ratio_err = 0.0f;
    self->_inited = false;
}
