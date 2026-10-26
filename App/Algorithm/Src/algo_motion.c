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
int algo_motion_vernier(float rotor, float vernier, float phase, float reducer_ratio,
                        float output_ratio, float* shaft_rad, float* residual_out) {
    if (residual_out != NULL) {
        *residual_out = 0.25f;
    }
    if (!shaft_rad || !foc_finite(rotor) || !foc_finite(vernier) || !foc_finite(phase)
        || !foc_finite(reducer_ratio) || !foc_finite(output_ratio) || (reducer_ratio < 1.0f)
        || (output_ratio <= 0.0f) || (output_ratio >= 1.0f) || (phase < 0.0f)
        || (phase >= 1.0f / reducer_ratio) || (rotor < 0.0f) || (rotor >= 1.0f)
        || (vernier < 0.0f) || (vernier >= 1.0f))
        return -1;
    /* 游标一致性：reducer 必须是 output_ratio 的游标周期 1/(1-output_ratio) */
    float expected_reducer = 1.0f / (1.0f - output_ratio);
    float reducer_delta = reducer_ratio - expected_reducer;
    if (!foc_finite(expected_reducer) || (reducer_delta > 1.0e-3f)
        || (reducer_delta < -1.0e-3f))
        return -1;

    float estimate = reducer_ratio * motion_wrap(output_ratio * rotor - vernier + phase);
    if (!foc_finite(estimate))
        return -1;
    float nearest = (float)(uint32_t)(estimate + .5f);
    float residual = estimate - nearest;
    if (residual > .25f || residual < -.25f)
        return -1;
    if (residual_out != NULL) {
        *residual_out = residual < 0.0f ? -residual : residual;
    }
    if (nearest >= reducer_ratio)
        nearest = 0;
    *shaft_rad = (nearest + rotor) * (FOC_TWO_PI_F / reducer_ratio);
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
    /* 条件积分：非饱和（|raw|<limit）或误差反向（回退）才积分；外部饱和仅允许回退 */
    bool unwind = (raw > 0 && error < 0) || (raw < 0 && error > 0);
    if (unwind || (!pi->saturated && raw > -limit && raw < limit)) {
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

FOC_ATTR_RAMFUNC
float algo_motion_ramp(float current, float target, float rate, float dt) {
    if (!foc_finite(current) || !foc_finite(target)) {
        return 0.0f;
    }
    if (!foc_finite(rate) || rate <= 0.0f) {
        return target;
    }
    if (!foc_finite(dt) || dt <= 0.0f) {
        return current;
    }

    float step = rate * dt;
    if (!foc_finite(step) || step <= 0.0f) {
        return current;
    }
    float error = target - current;
    if (error > step) {
        return current + step;
    }
    if (error < -step) {
        return current - step;
    }
    return target;
}

FOC_ATTR_RAMFUNC
float algo_motion_limit_vel(float torque, float speed, float vel_limit, float gain, bool* limited) {
    if (limited != NULL) {
        *limited = false;
    }
    if (!foc_finite(torque) || !foc_finite(speed) || !foc_finite(vel_limit)
        || !foc_finite(gain) || (vel_limit <= 0.0f) || (gain <= 0.0f)) {
        return torque;
    }

    float torque_max = (vel_limit - speed) * gain;
    float torque_min = (-vel_limit - speed) * gain;
    if (!foc_finite(torque_max) || !foc_finite(torque_min)) {
        return torque;
    }
    if (torque > torque_max) {
        if (limited != NULL) {
            *limited = true;
        }
        return torque_max;
    }
    if (torque < torque_min) {
        if (limited != NULL) {
            *limited = true;
        }
        return torque_min;
    }
    return torque;
}

FOC_ATTR_RAMFUNC
void algo_motion_pos_filter_step(
    algo_motion_pos_filter_t* f, float input_pos, float input_vel, float bandwidth_rad_s,
    float dt) {
    if (!f || !foc_finite(input_pos) || !foc_finite(input_vel) || !foc_finite(bandwidth_rad_s)
        || !foc_finite(dt) || dt <= 0)
        return;
    if (bandwidth_rad_s <= 0) { /* 直通：滤波关闭 */
        f->pos_setpoint_rad = input_pos;
        f->vel_setpoint_rad_s = input_vel;
        f->accel_setpoint_rad_s2 = 0.0f;
        return;
    }
    /* ODrive update_filter_gains：bw 钳位≤0.25/dt，ki=2bw，kp=0.25ki²=bw²（临界阻尼） */
    float bandwidth = bandwidth_rad_s > 0.25f / dt ? 0.25f / dt : bandwidth_rad_s;
    if (!foc_finite(f->pos_setpoint_rad) || !foc_finite(f->vel_setpoint_rad_s)) {
        f->pos_setpoint_rad = input_pos;
        f->vel_setpoint_rad_s = input_vel;
        f->accel_setpoint_rad_s2 = 0.0f;
        return;
    }
    float ki = 2.0f * bandwidth;
    float kp = 0.25f * ki * ki;
    /* 单圈绝对模式：位置差取模最短路径（目标跨零点平滑，不绕大圈） */
    float delta_pos = foc_wrap_pm_pi(input_pos - f->pos_setpoint_rad);
    float accel = kp * delta_pos + ki * (input_vel - f->vel_setpoint_rad_s);
    f->accel_setpoint_rad_s2 = accel;
    f->vel_setpoint_rad_s += dt * accel;
    f->pos_setpoint_rad += dt * f->vel_setpoint_rad_s;
}
FOC_ATTR_RAMFUNC
float algo_motion_position(float pos_setpoint, float pos_est, float pos_gain, float vel_ff) {
    if (!foc_finite(pos_setpoint) || !foc_finite(pos_est) || !foc_finite(pos_gain)
        || !foc_finite(vel_ff))
        return 0;
    /* 单圈绝对模式：位置误差取模最短路径（±π，ODrive circular 语义） */
    return vel_ff + pos_gain * foc_wrap_pm_pi(pos_setpoint - pos_est);
}
