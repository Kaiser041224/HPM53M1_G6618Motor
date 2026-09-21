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
 * @brief 初始化
 */
static int foc_angle_init(foc_angle_t* self, const foc_angle_cfg_t* cfg) {
    if ((self == NULL) || (cfg == NULL)) {
        return -1;
    }
    if ((cfg->pole_pairs == 0U) || (cfg->sample_time_s <= 0.0f)) {
        return -1;
    }
    if ((cfg->direction != 1.0f) && (cfg->direction != -1.0f)) {
        return -1;
    }

    self->_p = (float)cfg->pole_pairs;
    self->_dir = cfg->direction;
    self->_offset = cfg->offset_rad;
    self->_ts = cfg->sample_time_s;
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
 * @brief 更新零点/方向
 */
static void foc_angle_set_offset(foc_angle_t* self, float offset_rad, float direction) {
    if (self == NULL) {
        return;
    }
    self->_offset = offset_rad;
    if ((direction == 1.0f) || (direction == -1.0f)) {
        self->_dir = direction;
    }
}

/**
 * @brief 单步
 */
FOC_ATTR_RAMFUNC
static float foc_angle_step(foc_angle_t* self, float theta_m_raw_rad, float* omega_e_out) {
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
    } else {
        float dtheta = foc_wrap_pm_pi(theta_e - self->_theta_e_prev);
        float omega_meas = dtheta / self->_ts;
        self->_omega_e += self->_alpha * (omega_meas - self->_omega_e);
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
    self->_theta_e_prev = 0.0f;
    self->_omega_e = 0.0f;
    self->_primed = false;
    self->_inited = false;
}
