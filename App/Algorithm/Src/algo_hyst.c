/**
 * @file    algo_hyst.c
 * @brief   施密特触发器（滞回比较）实现
 * @author  Kaiser
 *
 * Schmitt Trigger Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_hyst.h"

#include <stddef.h>

static int algo_hyst_init_impl(algo_hyst_t* self, const algo_hyst_cfg_t* cfg);
static bool algo_hyst_step_impl(algo_hyst_t* self, float x);
static void algo_hyst_reset_impl(algo_hyst_t* self);
static bool algo_hyst_get_state_impl(const algo_hyst_t* self);
static void algo_hyst_set_state_impl(algo_hyst_t* self, bool state);
static int algo_hyst_set_thresholds_impl(algo_hyst_t* self, float lower, float upper);

/* ── Init ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化施密特触发器
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_hyst_init_impl(algo_hyst_t* self, const algo_hyst_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;

    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;
    if (!algo_hyst_finite(cfg->upper))
        return -3;
    if (!algo_hyst_finite(cfg->lower))
        return -4;
    if (cfg->upper <= cfg->lower)
        return -5;

    self->_upper = cfg->upper;
    self->_lower = cfg->lower;
    self->_state = cfg->init_state;
    self->_init_state = cfg->init_state;
    self->_inited = true;

    return 0;
}

/* ── Step ─────────────────────────────────────────────────────────────── */

/**
 * @brief 单步比较（滞回）
 * @param self 对象
 * @param x 输入
 * @return 当前状态
 */
static bool algo_hyst_step_impl(algo_hyst_t* self, float x) {
    if (self == NULL || !self->_inited)
        return false;

    if (!algo_hyst_finite(x))
        return self->_state;

    if (x > self->_upper)
        self->_state = true;
    else if (x < self->_lower)
        self->_state = false;

    return self->_state;
}

/* ── Reset ────────────────────────────────────────────────────────────── */

/**
 * @brief 复位到初始状态
 * @param self 对象
 */
static void algo_hyst_reset_impl(algo_hyst_t* self) {
    if (self == NULL || !self->_inited)
        return;
    self->_state = self->_init_state;
}

/* ── get_state ────────────────────────────────────────────────────────── */

/**
 * @brief 读取当前状态
 * @param self 对象
 * @return 当前状态
 */
static bool algo_hyst_get_state_impl(const algo_hyst_t* self) {
    if (self == NULL || !self->_inited)
        return false;
    return self->_state;
}

/* ── set_state ────────────────────────────────────────────────────────── */

/**
 * @brief 强制设置当前状态
 * @param self 对象
 * @param state 目标状态
 */
static void algo_hyst_set_state_impl(algo_hyst_t* self, bool state) {
    if (self == NULL || !self->_inited)
        return;
    self->_state = state;
}

/* ── set_thresholds ───────────────────────────────────────────────────── */

/**
 * @brief 更新上下阈值
 * @param self 对象
 * @param lower 下阈值
 * @param upper 上阈值
 * @return 0 成功；负数错误码
 */
static int algo_hyst_set_thresholds_impl(algo_hyst_t* self, float lower, float upper) {
    if (self == NULL || !self->_inited)
        return -1;
    if (!algo_hyst_finite(lower))
        return -2;
    if (!algo_hyst_finite(upper))
        return -3;
    if (upper <= lower)
        return -4;

    self->_lower = lower;
    self->_upper = upper;
    return 0;
}

/* ── Constructor ──────────────────────────────────────────────────────── */

/**
 * @brief 构造施密特触发器对象
 * @param self 对象
 */
void algo_hyst_ctor(algo_hyst_t* self) {
    if (self == NULL)
        return;

    self->init = algo_hyst_init_impl;
    self->step = algo_hyst_step_impl;
    self->reset = algo_hyst_reset_impl;
    self->get_state = algo_hyst_get_state_impl;
    self->set_state = algo_hyst_set_state_impl;
    self->set_thresholds = algo_hyst_set_thresholds_impl;
    self->_upper = 0.0f;
    self->_lower = 0.0f;
    self->_state = false;
    self->_init_state = false;
    self->_inited = false;
}
