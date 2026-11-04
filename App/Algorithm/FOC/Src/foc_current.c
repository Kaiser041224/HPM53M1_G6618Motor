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
        || !foc_finite(cfg->lambda) || !foc_finite(cfg->r_ohm)) {
        return -1;
    }
    if ((cfg->sample_time_s <= 0.0f) || (cfg->kp < 0.0f) || (cfg->ki < 0.0f)
        || (cfg->r_ohm < 0.0f)) {
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
    self->_wl_ff = (self->_decouple || (cfg->wl_ff_en != 0U)) ? 1U : 0U;
    self->_bemf_ff = (self->_decouple || (cfg->bemf_ff_en != 0U)) ? 1U : 0U;
    self->_ld = cfg->l_d;
    self->_lq = cfg->l_q;
    self->_lambda = cfg->lambda;
    self->_r = cfg->r_ohm;
    self->_r_ff = (cfg->r_ff_en != 0U) ? 1U : 0U;
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
 * @brief 更新增益（25kHz ISR 每拍热更新 → ILM）
 */
FOC_ATTR_RAMFUNC
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
 * @brief 更新电机模型（R/L/λ），不重置积分器
 * @return 0 = 成功；-1 = 非法值（保留旧模型）
 */
FOC_ATTR_RAMFUNC
static int foc_current_set_model(foc_current_t* self, float r_ohm, float l_d, float l_q,
                                 float lambda) {
    if (self == NULL) {
        return -1;
    }
    if (!foc_finite(r_ohm) || (r_ohm < 0.0f) || !foc_finite(l_d) || (l_d < 0.0f)
        || !foc_finite(l_q) || (l_q < 0.0f) || !foc_finite(lambda) || (lambda < 0.0f)) {
        return -1;
    }
    self->_r = r_ohm;
    self->_ld = l_d;
    self->_lq = l_q;
    self->_lambda = lambda;
    return 0;
}

/**
 * @brief 运行中开关解耦前馈（25kHz ISR 每拍调用 → ILM）
 */
FOC_ATTR_RAMFUNC
static void foc_current_set_decoupling(foc_current_t* self, uint8_t enable) {
    if (self == NULL) {
        return;
    }
    self->_decouple = (enable != 0U) ? 1U : 0U;
    self->_wl_ff = self->_decouple;
    self->_bemf_ff = self->_decouple;
}

/**
 * @brief 运行中开关 ωL 交叉耦合前馈（25kHz ISR 每拍调用 → ILM）
 */
FOC_ATTR_RAMFUNC
static void foc_current_set_wl_ff(foc_current_t* self, uint8_t enable) {
    if (self == NULL) {
        return;
    }
    self->_wl_ff = (enable != 0U) ? 1U : 0U;
}

/**
 * @brief 运行中开关 bEMF 前馈（25kHz ISR 每拍调用 → ILM）
 */
FOC_ATTR_RAMFUNC
static void foc_current_set_bemf_ff(foc_current_t* self, uint8_t enable) {
    if (self == NULL) {
        return;
    }
    self->_bemf_ff = (enable != 0U) ? 1U : 0U;
}

/**
 * @brief 运行中开关 R·I 前馈（25kHz ISR 每拍调用 → ILM）
 */
FOC_ATTR_RAMFUNC
static void foc_current_set_r_ff(foc_current_t* self, uint8_t enable) {
    if (self == NULL) {
        return;
    }
    self->_r_ff = (enable != 0U) ? 1U : 0U;
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

    /* 1) 电流矢量限幅：Id 优先 2-norm（ODrive Motor::update 语义）。
     *   先给 d 轴占预算，再让 q 轴使用剩余预算；适合 motor id / 弱磁场景。 */
    imax = (foc_finite(in->i_max) && (in->i_max > 0.0f)) ? in->i_max : 0.0f;
    if (imax <= 0.0f) {
        id_ref = 0.0f;
        iq_ref = 0.0f;
    } else {
        float iq_limit;

        if (id_ref > imax) {
            id_ref = imax;
        } else if (id_ref < -imax) {
            id_ref = -imax;
        }
        iq_limit = imax * imax - id_ref * id_ref;
        iq_limit = (iq_limit > 0.0f) ? foc_sqrtf(iq_limit) : 0.0f;
        if (iq_ref > iq_limit) {
            iq_ref = iq_limit;
        } else if (iq_ref < -iq_limit) {
            iq_ref = -iq_limit;
        }
    }
    out->i_d_ref_lim = id_ref;
    out->i_q_ref_lim = iq_ref;

    /* 2) 误差 + PI */
    ed = id_ref - id_fb;
    eq = iq_ref - iq_fb;
    vd = self->_kp * ed + self->_integ_d;
    vq = self->_kp * eq + self->_integ_q;

    /* 3a) R·I 前馈：PI 后、电压限幅前，使用限幅后的给定。
     *     R 在 init 期固定；默认关闭，避免未验证的 Rs 直接注入电压。 */
    if (self->_r_ff != 0U) {
        vd += self->_r * out->i_d_ref_lim;
        vq += self->_r * out->i_q_ref_lim;
    }

    /* 3b) ωL / bEMF 独立前馈（默认关闭；非有限 ωe 按 0 处理） */
    if ((self->_wl_ff != 0U) || (self->_bemf_ff != 0U)) {
        float omega_e = foc_finite(in->omega_e_rad_s) ? in->omega_e_rad_s : 0.0f;

        /* 交叉耦合前馈使用限幅后的给定电流：反馈电流含开关纹波且相对给定有
         * 相位滞后，用作前馈会把噪声和延迟注入 vd/vq。 */
        if (self->_wl_ff != 0U) {
            vd -= omega_e * self->_lq * out->i_q_ref_lim;
            vq += omega_e * self->_ld * out->i_d_ref_lim;
        }
        if (self->_bemf_ff != 0U) {
            vq += omega_e * self->_lambda;
        }
    }

    /* 4) 圆形电压限幅（保角；常用路径仅平方比较，避免 sqrtf） */
    vmax = (foc_finite(in->v_max) && (in->v_max > 0.0f)) ? in->v_max : 0.0f;
    mag2 = vd * vd + vq * vq;
    if (mag2 > vmax * vmax) {
        mag = foc_sqrtf(mag2);
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
    self->set_model = foc_current_set_model;
    self->set_decoupling = foc_current_set_decoupling;
    self->set_r_ff = foc_current_set_r_ff;
    self->set_wl_ff = foc_current_set_wl_ff;
    self->set_bemf_ff = foc_current_set_bemf_ff;
    self->_kp = 0.0f;
    self->_ki = 0.0f;
    self->_ts = 1.0f;
    self->_kits = 0.0f;
    self->_decouple = 0U;
    self->_wl_ff = 0U;
    self->_bemf_ff = 0U;
    self->_ld = 0.0f;
    self->_lq = 0.0f;
    self->_lambda = 0.0f;
    self->_r = 0.0f;
    self->_r_ff = 0U;
    self->_aw_decay = 0.99f;
    self->_integ_d = 0.0f;
    self->_integ_q = 0.0f;
    self->_inited = false;
}
