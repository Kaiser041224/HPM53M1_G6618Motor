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
    if (!foc_finite(cfg->sample_time_s) || !foc_finite(cfg->kp) || !foc_finite(cfg->ki)
        || !foc_finite(cfg->aw_decay) || !foc_finite(cfg->l_d) || !foc_finite(cfg->l_q)
        || !foc_finite(cfg->lambda)) {
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
    self->_kits = cfg->ki * cfg->sample_time_s;
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
        self->_kits = ki * self->_ts;
    }
}

/**
 * @brief 单步
 */
FOC_ATTR_RAMFUNC
static int foc_current_step(foc_current_t* self, const foc_current_in_t* in,
                            foc_current_out_t* out) {
    float id_ref, iq_ref, id_fb, iq_fb, ed, eq, vd, vq, mag2, mag, vmax, imax;
    bool saturated = false;
    bool fb_invalid;

    if ((self == NULL) || (in == NULL) || (out == NULL) || !self->_inited) {
        return -1;
    }

    /* 输入净化：非有限给定按 0；反馈不可信时置 fb_invalid（后续零给定 + 零电压） */
    fb_invalid = (!foc_finite(in->i_d_a) || !foc_finite(in->i_q_a));
    id_ref = foc_finite(in->i_d_ref) ? in->i_d_ref : 0.0f;
    iq_ref = foc_finite(in->i_q_ref) ? in->i_q_ref : 0.0f;
    id_fb = foc_finite(in->i_d_a) ? in->i_d_a : 0.0f;
    iq_fb = foc_finite(in->i_q_a) ? in->i_q_a : 0.0f;
    if (fb_invalid) {
        id_ref = 0.0f; /* 反馈不可信：不得据此调压（零给定） */
        iq_ref = 0.0f;
    }

    /* 1) 电流矢量限幅（圆形，保角） */
    imax = (foc_finite(in->i_max) && (in->i_max > 0.0f)) ? in->i_max : 0.0f;
    {
        float imag2 = id_ref * id_ref + iq_ref * iq_ref;
        if (imag2 > imax * imax) {
            float imag = sqrtf(imag2);
            if (imag > 0.0f) {
                float k = imax / imag;
                id_ref *= k;
                iq_ref *= k;
            }
        }
    }
    out->i_d_ref_lim = id_ref;
    out->i_q_ref_lim = iq_ref;

    /* 2) 误差 + PI */
    ed = id_ref - id_fb;
    eq = iq_ref - iq_fb;
    vd = self->_kp * ed + self->_integ_d;
    vq = self->_kp * eq + self->_integ_q;

    /* 3) 解耦前馈（结构预留；V1 默认关闭；非有限 ωe 按 0 处理） */
    if (self->_decouple != 0U) {
        float omega_e = foc_finite(in->omega_e_rad_s) ? in->omega_e_rad_s : 0.0f;

        vd -= omega_e * self->_lq * iq_fb;
        vq += omega_e * (self->_ld * id_fb + self->_lambda);
    }

    /* 4) 圆形电压限幅（保角；常用路径仅平方比较，避免 sqrtf） */
    vmax = (foc_finite(in->v_max) && (in->v_max > 0.0f)) ? in->v_max : 0.0f;
    mag2 = vd * vd + vq * vq;
    if (mag2 > vmax * vmax) {
        mag = sqrtf(mag2);
        if (mag > 0.0f) {
            float k = vmax / mag;
            vd *= k;
            vq *= k;
            saturated = true;
        }
    }

    /* 4b) 反馈不可信：强制零电压并按饱和处理（本拍积分衰减，不产生驱动） */
    if (fb_invalid) {
        vd = 0.0f;
        vq = 0.0f;
        saturated = true;
    }

    /* 5) 抗饱和：未饱和累加 / 饱和衰减；积分单独限幅 */
    if (!saturated) {
        self->_integ_d += self->_kits * ed;
        self->_integ_q += self->_kits * eq;
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

    /* 6) 输出防线：任何非有限结果 → 零输出（不得送硬件） */
    if (!foc_finite(vd) || !foc_finite(vq)) {
        vd = 0.0f;
        vq = 0.0f;
        saturated = true;
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
    self->_kits = 0.0f;
    self->_decouple = 0U;
    self->_ld = 0.0f;
    self->_lq = 0.0f;
    self->_lambda = 0.0f;
    self->_aw_decay = 0.99f;
    self->_integ_d = 0.0f;
    self->_integ_q = 0.0f;
    self->_inited = false;
}
