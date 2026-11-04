/**
 * @file    algo_speed_observer.c
 * @brief   转子机械角二阶跟踪观测器（PLL）实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_speed_observer.h"

#include "foc_math.h"

#include <math.h>
#include <stddef.h>

#define ALGO_SPEED_OBSERVER_DT_MIN_S (1.0e-5f)
#define ALGO_SPEED_OBSERVER_DT_MAX_S (5.0e-3f)

/**
 * @brief 初始化
 */
static int algo_speed_observer_init_impl(algo_speed_observer_t* self,
                                         const algo_speed_observer_cfg_t* cfg) {
    if ((self == NULL) || (cfg == NULL)) {
        return -1;
    }
    if (!foc_finite(cfg->bandwidth_rad_s) || (cfg->bandwidth_rad_s <= 0.0f)
        || !foc_finite(cfg->sample_time_s) || (cfg->sample_time_s <= 0.0f)
        || !foc_finite(cfg->max_speed_rad_s) || (cfg->max_speed_rad_s <= 0.0f)) {
        return -1;
    }

    self->_kp = 2.0f * cfg->bandwidth_rad_s;
    self->_ki = cfg->bandwidth_rad_s * cfg->bandwidth_rad_s;
    self->_ts = cfg->sample_time_s;
    self->_max_speed = cfg->max_speed_rad_s;
    self->_theta = 0.0f;
    self->_omega = 0.0f;
    self->_primed = false;
    self->_inited = true;
    return 0;
}

/**
 * @brief 单步：theta_meas 为未加零点的机械角 [rad]
 */
FOC_ATTR_RAMFUNC
static float algo_speed_observer_step_impl(algo_speed_observer_t* self, float theta_meas_rad,
                                           float dt_s, float* omega_out) {
    if (omega_out != NULL) {
        *omega_out = 0.0f;
    }
    if ((self == NULL) || !self->_inited) {
        return 0.0f;
    }

    if (!foc_finite(theta_meas_rad) || !foc_finite(dt_s) || (dt_s <= ALGO_SPEED_OBSERVER_DT_MIN_S)
        || (dt_s > ALGO_SPEED_OBSERVER_DT_MAX_S)) {
        if (omega_out != NULL) {
            *omega_out = self->_omega;
        }
        return self->_omega;
    }

    if (!self->_primed) {
        self->_theta = foc_wrap_2pi(theta_meas_rad);
        self->_omega = 0.0f;
        self->_primed = true;
        if (omega_out != NULL) {
            *omega_out = 0.0f;
        }
        return 0.0f;
    }

    /* NCO 预测 → 相位检测 → 相位校正 → 速度积分 */
    self->_theta = foc_wrap_2pi(self->_theta + dt_s * self->_omega);
    float err = foc_wrap_pm_pi(theta_meas_rad - self->_theta);
    self->_theta = foc_wrap_2pi(self->_theta + dt_s * self->_kp * err);
    self->_omega += dt_s * self->_ki * err;

    if (!foc_finite(self->_theta) || !foc_finite(self->_omega)) {
        self->_theta = foc_wrap_2pi(theta_meas_rad);
        self->_omega = 0.0f;
        if (omega_out != NULL) {
            *omega_out = 0.0f;
        }
        return 0.0f;
    }

    if (self->_omega > self->_max_speed) {
        self->_omega = self->_max_speed;
    } else if (self->_omega < -self->_max_speed) {
        self->_omega = -self->_max_speed;
    }

    if (omega_out != NULL) {
        *omega_out = self->_omega;
    }
    return self->_omega;
}

/**
 * @brief 复位
 */
static void algo_speed_observer_reset_impl(algo_speed_observer_t* self) {
    if (self == NULL) {
        return;
    }
    self->_theta = 0.0f;
    self->_omega = 0.0f;
    self->_primed = false;
}

/**
 * @brief 读取机械速度
 */
static float algo_speed_observer_get_omega_impl(const algo_speed_observer_t* self) {
    if ((self == NULL) || !self->_inited) {
        return 0.0f;
    }
    return self->_omega;
}

/**
 * @brief 读取机械角
 */
static float algo_speed_observer_get_theta_impl(const algo_speed_observer_t* self) {
    if ((self == NULL) || !self->_inited) {
        return 0.0f;
    }
    return self->_theta;
}

/**
 * @brief 构造
 */
void algo_speed_observer_ctor(algo_speed_observer_t* self) {
    if (self == NULL) {
        return;
    }
    self->init = algo_speed_observer_init_impl;
    self->step = algo_speed_observer_step_impl;
    self->reset = algo_speed_observer_reset_impl;
    self->get_omega = algo_speed_observer_get_omega_impl;
    self->get_theta = algo_speed_observer_get_theta_impl;
    self->_kp = 0.0f;
    self->_ki = 0.0f;
    self->_ts = 0.0f;
    self->_max_speed = 0.0f;
    self->_theta = 0.0f;
    self->_omega = 0.0f;
    self->_primed = false;
    self->_inited = false;
}
