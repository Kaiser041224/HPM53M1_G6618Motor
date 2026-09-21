/**
 * @file    foc_angle.c
 * @brief   电角度链实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "foc_angle.h"

/**
 * @brief 方向合法性判定（±1.0，容差 1e-3）
 * @param direction 方向值
 * @return true = 合法（|‖direction‖ − 1| ≤ 1e-3）
 */
static bool foc_angle_direction_valid(float direction) {
    return (foc_finite(direction) && (fabsf(fabsf(direction) - 1.0f) <= 1e-3f));
}

/** dt 合理范围 [s]：超出视为节拍异常（10µs ~ 5ms） */
#define FOC_ANGLE_DT_MIN_S (1.0e-5f)
#define FOC_ANGLE_DT_MAX_S (5.0e-3f)

/**
 * @brief 初始化
 */
static int foc_angle_init(foc_angle_t* self, const foc_angle_cfg_t* cfg) {
    if ((self == NULL) || (cfg == NULL)) {
        return -1;
    }
    if ((cfg->pole_pairs == 0U) || (cfg->sample_time_s <= 0.0f)) {
        return -1;
    }
    if (!foc_finite(cfg->sample_time_s) || !foc_finite(cfg->speed_lpf_hz)
        || !foc_finite(cfg->offset_rad) || (cfg->speed_lpf_hz < 0.0f)) {
        return -1;
    }
    if (!foc_angle_direction_valid(cfg->direction)) {
        return -1;
    }

    self->_p = (float)cfg->pole_pairs;
    self->_dir = (cfg->direction > 0.0f) ? 1.0f : -1.0f;
    self->_offset = cfg->offset_rad;
    self->_ts = cfg->sample_time_s;
    self->_inv_ts = 1.0f / cfg->sample_time_s;
    if (cfg->speed_lpf_hz > 0.0f) {
        float rc = 1.0f / (FOC_TWO_PI_F * cfg->speed_lpf_hz);
        self->_alpha = cfg->sample_time_s / (cfg->sample_time_s + rc);
    } else {
        self->_alpha = 1.0f;
    }
    self->_theta_e_prev = 0.0f;
    self->_omega_e = 0.0f;
    self->_primed = false;
    self->_inited = true;
    return 0;
}

/**
 * @brief 复位
 */
static void foc_angle_reset(foc_angle_t* self) {
    if (self == NULL) {
        return;
    }
    self->_theta_e_prev = 0.0f;
    self->_omega_e = 0.0f;
    self->_primed = false;
}

/**
 * @brief 更新零点/方向（并重新起算 ωe 差分，避免跳变尖峰）
 */
static void foc_angle_set_offset(foc_angle_t* self, float offset_rad, float direction) {
    if ((self == NULL) || !foc_finite(offset_rad)) {
        return;
    }
    self->_offset = offset_rad;
    if (foc_angle_direction_valid(direction)) {
        self->_dir = (direction > 0.0f) ? 1.0f : -1.0f;
    }
    /* 零点/方向变更 → θe 不连续：清除差分历史与 ωe，下一拍从零起算 */
    self->_theta_e_prev = 0.0f;
    self->_omega_e = 0.0f;
    self->_primed = false;
}

/**
 * @brief 单步
 */
FOC_ATTR_RAMFUNC
static float foc_angle_step(foc_angle_t* self, float theta_m_raw_rad, float dt_s,
                            float* omega_e_out) {
    float theta_e;

    if ((self == NULL) || !self->_inited) {
        if (omega_e_out != NULL) {
            *omega_e_out = 0.0f;
        }
        return 0.0f;
    }

    if (!foc_finite(theta_m_raw_rad)) {
        if (omega_e_out != NULL) {
            *omega_e_out = self->_omega_e;
        }
        return self->_theta_e_prev;
    }

    theta_e = foc_wrap_2pi(self->_p * self->_dir * theta_m_raw_rad - self->_offset);

    if (!self->_primed) {
        self->_primed = true;
        self->_omega_e = 0.0f;
    } else if (foc_finite(dt_s) && (dt_s >= FOC_ANGLE_DT_MIN_S) && (dt_s <= FOC_ANGLE_DT_MAX_S)) {
        /* 按实测 dt 换算（主循环丢拍/长迭代时固定 1/25kHz 会给出 2~N 倍 ωe 尖峰，
         * 经限速判据放大为转矩断续 → 机械顿挫） */
        float dtheta = foc_wrap_pm_pi(theta_e - self->_theta_e_prev);
        float omega_meas = dtheta / dt_s;

        self->_omega_e += self->_alpha * (omega_meas - self->_omega_e);
    } else {
        /* dt 非法：保持上次估计（不更新差分历史仍继续） */
    }
    self->_theta_e_prev = theta_e;

    if (omega_e_out != NULL) {
        *omega_e_out = self->_omega_e;
    }
    return theta_e;
}

/**
 * @brief 构造
 */
void foc_angle_ctor(foc_angle_t* self) {
    if (self == NULL) {
        return;
    }
    self->init = foc_angle_init;
    self->step = foc_angle_step;
    self->reset = foc_angle_reset;
    self->set_offset = foc_angle_set_offset;
    self->_p = 0.0f;
    self->_dir = 1.0f;
    self->_offset = 0.0f;
    self->_alpha = 1.0f;
    self->_ts = 1.0f;
    self->_inv_ts = 1.0f;
    self->_theta_e_prev = 0.0f;
    self->_omega_e = 0.0f;
    self->_primed = false;
    self->_inited = false;
}
