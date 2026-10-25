/**
 * @file    app_cogging.c
 * @brief   齿槽/减速器周期波动补偿实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_cogging.h"

#include "app_motion.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "app_ramfunc.h"
#include "intf_sys.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static app_cogging_tables_t s_tables;
static const app_motor_params_t* s_motor_params;
static volatile bool s_master_enabled;
static volatile bool s_rotor_enabled;
static volatile bool s_output_enabled;
static volatile bool s_suspended;
static volatile bool s_speed_limited;
static volatile bool s_motion_active;
static volatile bool s_output_angle_valid;
static volatile float s_output_angle_rad;
static volatile float s_gain;

static bool cogging_finite(float value) { return isfinite(value); }

static void cogging_update_output_tracking(void) {
    bool required = s_master_enabled && s_output_enabled && s_tables.output_valid;
    app_motion_set_cogging_output_required(required);
}

void app_cogging_apply_defaults(void) {
    const app_software_params_t* software = app_software_params_current();
    bool rotor = software->cogging.rotor_enable && s_tables.rotor_valid;
    bool output = software->cogging.output_enable && s_tables.output_valid;
    float gain = software->cogging.gain;

    if (!cogging_finite(gain) || (gain < 0.0f)) {
        gain = 0.0f;
    } else if (gain > 2.0f) {
        gain = 2.0f;
    }
    s_rotor_enabled = rotor;
    s_output_enabled = output;
    s_gain = gain;
    s_master_enabled = (software->cogging.enable != 0U) && (rotor || output);
    cogging_update_output_tracking();
}

void app_cogging_init(void) {
    memset(&s_tables, 0, sizeof(s_tables));
    s_motor_params = app_motor_params_current();
    s_master_enabled = false;
    s_rotor_enabled = false;
    s_output_enabled = false;
    s_suspended = false;
    s_speed_limited = false;
    s_motion_active = false;
    s_output_angle_valid = false;
    s_output_angle_rad = 0.0f;
    s_gain = 1.0f;
}

int app_cogging_set_tables(const app_cogging_tables_t* tables) {
    if (tables == NULL) {
        return -1;
    }
    app_cogging_tables_t copy = *tables;
    uint32_t state = intf_sys_irq_save();
    s_tables = copy;
    if (!s_tables.rotor_valid && !s_tables.output_valid) {
        s_master_enabled = false;
        s_rotor_enabled = false;
        s_output_enabled = false;
    } else {
        if (!s_tables.rotor_valid) {
            s_rotor_enabled = false;
        }
        if (!s_tables.output_valid) {
            s_output_enabled = false;
        }
    }
    intf_sys_irq_restore(state);
    cogging_update_output_tracking();
    return 0;
}

const app_cogging_tables_t* app_cogging_get_tables(void) { return &s_tables; }

void app_cogging_clear_tables(void) {
    uint32_t state = intf_sys_irq_save();
    memset(&s_tables, 0, sizeof(s_tables));
    s_master_enabled = false;
    s_rotor_enabled = false;
    s_output_enabled = false;
    intf_sys_irq_restore(state);
    cogging_update_output_tracking();
}

int app_cogging_set_master(bool enable) {
    if (enable && !s_tables.rotor_valid && !s_tables.output_valid) {
        return -1;
    }
    if (enable && !s_rotor_enabled && !s_output_enabled) {
        /* 首次开启总开关时自动启用已有表；用户仍可单独关闭某张表。 */
        s_rotor_enabled = s_tables.rotor_valid;
        s_output_enabled = s_tables.output_valid;
    }
    s_master_enabled = enable;
    cogging_update_output_tracking();
    return 0;
}

int app_cogging_set_rotor_enable(bool enable) {
    if (enable && !s_tables.rotor_valid) {
        return -1;
    }
    s_rotor_enabled = enable;
    return 0;
}

int app_cogging_set_output_enable(bool enable) {
    if (enable && !s_tables.output_valid) {
        return -1;
    }
    s_output_enabled = enable;
    cogging_update_output_tracking();
    return 0;
}

int app_cogging_set_gain(float gain) {
    if (!cogging_finite(gain) || (gain < 0.0f) || (gain > 2.0f)) {
        return -1;
    }
    s_gain = gain;
    return 0;
}

void app_cogging_suspend(bool suspend) { s_suspended = suspend; }
void app_cogging_set_speed_limited(bool limited) { s_speed_limited = limited; }
void app_cogging_set_motion_active(bool active) { s_motion_active = active; }

void app_cogging_set_output_angle(float angle_rad, bool valid) {
    s_output_angle_rad = cogging_finite(angle_rad) ? angle_rad : 0.0f;
    s_output_angle_valid = valid && cogging_finite(angle_rad);
}

APP_ATTR_RAMFUNC
float app_cogging_iq(float rotor_rad) {
    if (!s_master_enabled || s_suspended || s_speed_limited || !s_motion_active
        || !cogging_finite(rotor_rad)) {
        return 0.0f;
    }
    const app_motor_params_t* motor = s_motor_params;
    if ((motor == NULL) || !cogging_finite(motor->kt_nm_per_a)
        || (motor->kt_nm_per_a <= 0.0f) || !cogging_finite(motor->reducer_ratio)
        || (motor->reducer_ratio <= 0.0f)) {
        return 0.0f;
    }

    float iq = 0.0f;
    if (s_rotor_enabled && s_tables.rotor_valid) {
        float torque = algo_cogging_interp_i16(
            s_tables.rotor, ALGO_COGGING_ROTOR_POINTS, APP_COGGING_ROTOR_SCALE_NM, rotor_rad);
        iq += torque / motor->kt_nm_per_a;
    }
    if (s_output_enabled && s_tables.output_valid && s_output_angle_valid) {
        float torque = algo_cogging_interp_i16(
            s_tables.output, ALGO_COGGING_OUTPUT_POINTS, APP_COGGING_OUTPUT_SCALE_NM,
            s_output_angle_rad);
        iq += torque / (motor->reducer_ratio * motor->kt_nm_per_a);
    }
    iq *= s_gain;
    return cogging_finite(iq) ? iq : 0.0f;
}

void app_cogging_get_snapshot(app_cogging_snapshot_t* out) {
    if (out == NULL) {
        return;
    }
    uint32_t state = intf_sys_irq_save();
    *out = (app_cogging_snapshot_t){
        .master_enabled = s_master_enabled,
        .rotor_enabled = s_rotor_enabled,
        .output_enabled = s_output_enabled,
        .rotor_valid = s_tables.rotor_valid,
        .output_valid = s_tables.output_valid,
        .suspended = s_suspended,
        .speed_limited = s_speed_limited,
        .output_angle_valid = s_output_angle_valid,
        .gain = s_gain,
        .output_angle_rad = s_output_angle_rad,
        .rotor_stats = s_tables.rotor_stats,
        .output_stats = s_tables.output_stats,
    };
    intf_sys_irq_restore(state);
}
