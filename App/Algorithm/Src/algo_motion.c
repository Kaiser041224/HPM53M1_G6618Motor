/** @file algo_motion.c
 * @brief 49/50游标及速度PI纯算法
 * @author Kaiser
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "algo_motion.h"
#include "foc_math.h"
/** @brief 已验证单圈输入产生的差值仅在(-1,1)内，无libm调用 */
static inline float motion_wrap(float value) { return value < 0 ? value + 1.0f : value; }
FOC_ATTR_RAMFUNC
int algo_motion_vernier(float rotor, float vernier, float phase, float* shaft_rad) {
    if (!shaft_rad || !foc_finite(rotor) || !foc_finite(vernier) || !foc_finite(phase) || phase < 0
        || phase >= .02f || rotor < 0 || rotor >= 1 || vernier < 0 || vernier >= 1)
        return -1;
    float estimate = 50.0f * motion_wrap(.98f * rotor - vernier + phase);
    float nearest = (float)(uint32_t)(estimate + .5f);
    if (estimate - nearest > .25f || nearest - estimate > .25f)
        return -1;
    if (nearest >= 50)
        nearest = 0;
    *shaft_rad = (nearest + rotor) * (FOC_TWO_PI_F / 50.0f);
    return 0;
}
FOC_ATTR_RAMFUNC
float algo_motion_speed(
    algo_motion_pi_t* pi, float target, float speed, float kp, float ki, float dt, float limit) {
    if (pi && !foc_finite(pi->integral)) {
        pi->integral = 0;
        return 0;
    }
    if (!pi || !foc_finite(target) || !foc_finite(speed) || !foc_finite(kp) || !foc_finite(ki)
        || !foc_finite(dt) || !foc_finite(limit) || kp < 0 || ki < 0 || dt <= 0 || dt > .001f
        || limit <= 0)
        return 0;
    float error = target - speed;
    float raw = kp * error + pi->integral;
    if (!foc_finite(error) || !foc_finite(raw)) {
        pi->integral = 0;
        return 0;
    }
    if ((raw > -limit && raw < limit) || (raw > 0 && error < 0) || (raw < 0 && error > 0)) {
        float integral = pi->integral + ki * error * dt;
        if (!foc_finite(integral)) {
            pi->integral = 0;
            return 0;
        }
        pi->integral = integral > limit ? limit : integral < -limit ? -limit : integral;
    }
    float output = kp * error + pi->integral;
    return output > limit ? limit : output < -limit ? -limit : output;
}
