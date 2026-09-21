/**
 * @file    foc_current.c
 * @brief   d/q 电流调节器实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "foc_current.h"

/**
 * @brief 初始化
 */
static int foc_current_init(foc_current_t* self, const foc_current_cfg_t* cfg) {
    if ((self == NULL) || (cfg == NULL)) {
        return -1;
    }
    if ((cfg->sample_time_s <= 0.0f) || (cfg->kp < 0.0f) || (cfg->ki < 0.0f)) {
        return -1;
    }
    if ((cfg->aw_decay <= 0.0f) || (cfg->aw_decay > 1.0f)) {
        return -1;
    }

    self->_kp = cfg->kp;
    self->_ki = cfg->ki;
    self->_ts = cfg->sample_time_s;
    self->_decouple = (cfg->decoupling_en != 0U) ? 1U : 0U;
    self->_ld = cfg->l_d;
    self->_lq = cfg->l_q;
    self->_lambda = cfg->lambda;
    self->_aw_decay = cfg->aw_decay;
    self->_integ_d = 0.0f;
    self->_integ_q = 0.0f;
    self->_inited = true;
    return 0;
}

/**
 * @brief 复位
 */
static void foc_current_reset(foc_current_t* self) {
    if (self == NULL) {
        return;
    }
    self->_integ_d = 0.0f;
    self->_integ_q = 0.0f;
}

/**
 * @brief 更新增益
 */
static void foc_current_set_gains(foc_current_t* self, float kp, float ki) {
    if (self == NULL) {
        return;
    }
    if ((kp >= 0.0f) && foc_finite(kp)) {
        self->_kp = kp;
    }
    if ((ki >= 0.0f) && foc_finite(ki)) {
        self->_ki = ki;
    }
}

/**
 * @brief 单步
 */
FOC_ATTR_RAMFUNC
static int foc_current_step(foc_current_t* self, const foc_current_in_t* in,
                            foc_current_out_t* out) {
    float id_ref, iq_ref, ed, eq, vd, vq, mag, vmax, imax;
    bool saturated = false;

    if ((self == NULL) || (in == NULL) || (out == NULL) || !self->_inited) {
        return -1;
    }

    id_ref = in->i_d_ref;
    iq_ref = in->i_q_ref;
    if (!foc_finite(id_ref) || !foc_finite(iq_ref) || !foc_finite(in->i_d_a)
        || !foc_finite(in->i_q_a)) {
        id_ref = 0.0f; /* 反馈异常：给定归零（安全） */
        iq_ref = 0.0f;
    }

    /* 1) 电流矢量限幅（圆形，保角） */
    imax = (in->i_max > 0.0f) ? in->i_max : 0.0f;
    {
        float imag = sqrtf(id_ref * id_ref + iq_ref * iq_ref);
        if ((imag > imax) && (imag > 0.0f)) {
            float k = imax / imag;
            id_ref *= k;
            iq_ref *= k;
        }
    }
    out->i_d_ref_lim = id_ref;
    out->i_q_ref_lim = iq_ref;

    /* 2) 误差 + PI */
    ed = id_ref - in->i_d_a;
    eq = iq_ref - in->i_q_a;
    vd = self->_kp * ed + self->_integ_d;
    vq = self->_kp * eq + self->_integ_q;

    /* 3) 解耦前馈（结构预留；V1 默认关闭） */
    if (self->_decouple != 0U) {
        vd -= in->omega_e_rad_s * self->_lq * in->i_q_a;
        vq += in->omega_e_rad_s * (self->_ld * in->i_d_a + self->_lambda);
    }

    /* 4) 圆形电压限幅（保角） */
    vmax = (in->v_max > 0.0f) ? in->v_max : 0.0f;
    mag = sqrtf(vd * vd + vq * vq);
    if ((mag > vmax) && (mag > 0.0f)) {
        float k = vmax / mag;
        vd *= k;
        vq *= k;
        saturated = true;
    }

    /* 5) 抗饱和：未饱和累加 / 饱和衰减；积分单独限幅 */
    if (!saturated) {
        self->_integ_d += self->_ki * self->_ts * ed;
        self->_integ_q += self->_ki * self->_ts * eq;
    } else {
        self->_integ_d *= self->_aw_decay;
        self->_integ_q *= self->_aw_decay;
    }
    if (self->_integ_d > vmax) {
        self->_integ_d = vmax;
    } else if (self->_integ_d < -vmax) {
        self->_integ_d = -vmax;
    }
    if (self->_integ_q > vmax) {
        self->_integ_q = vmax;
    } else if (self->_integ_q < -vmax) {
        self->_integ_q = -vmax;
    }

    out->v_d = vd;
    out->v_q = vq;
    out->saturated = saturated;
    return 0;
}

/**
 * @brief 构造
 */
void foc_current_ctor(foc_current_t* self) {
    if (self == NULL) {
        return;
    }
    self->init = foc_current_init;
    self->step = foc_current_step;
    self->reset = foc_current_reset;
    self->set_gains = foc_current_set_gains;
    self->_kp = 0.0f;
    self->_ki = 0.0f;
    self->_ts = 1.0f;
    self->_decouple = 0U;
    self->_ld = 0.0f;
    self->_lq = 0.0f;
    self->_lambda = 0.0f;
    self->_aw_decay = 0.99f;
    self->_integ_d = 0.0f;
    self->_integ_q = 0.0f;
    self->_inited = false;
}
