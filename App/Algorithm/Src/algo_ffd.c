/**
 * @file    algo_ffd.c
 * @brief   前馈补偿（线性 / 查表）实现
 * @author  Kaiser
 *
 * Feedforward Implementation
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "algo_ffd.h"

#include <stddef.h>
#include <stdint.h>

static int algo_ffd_init_impl(algo_ffd_t* self, const algo_ffd_cfg_t* cfg);
static float algo_ffd_step_impl(algo_ffd_t* self, float sp, float dv);
static void algo_ffd_reset_impl(algo_ffd_t* self);

static float algo_ffd_interp1d(const float* x_tbl, const float* y_tbl, uint16_t count, float x);

/* ── Init ─────────────────────────────────────────────────────────────── */

/**
 * @brief 初始化前馈对象
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_ffd_init_impl(algo_ffd_t* self, const algo_ffd_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;

    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;

    /* shared checks — both modes use all gains */
    if (!algo_ffd_finite(cfg->gain_sp))
        return -3;
    if (!algo_ffd_finite(cfg->gain_dv))
        return -4;
    if (!algo_ffd_finite(cfg->offset))
        return -5;

    switch (cfg->mode) {
    case ALGO_FFD_MODE_LINEAR: break;

    case ALGO_FFD_MODE_TABLE:
        if (cfg->x_tbl == NULL || cfg->y_tbl == NULL)
            return -6;
        if (cfg->n_pts < 2)
            return -7;
        for (uint16_t i = 0; i < cfg->n_pts; i++) {
            if (!algo_ffd_finite(cfg->x_tbl[i]))
                return -8;
            if (!algo_ffd_finite(cfg->y_tbl[i]))
                return -9;
        }
        for (uint16_t i = 1; i < cfg->n_pts; i++) {
            if (cfg->x_tbl[i] <= cfg->x_tbl[i - 1])
                return -10;
        }
        break;

    default: return -11;
    }

    self->_mode = cfg->mode;
    self->_gain_sp = cfg->gain_sp;
    self->_gain_dv = cfg->gain_dv;
    self->_offset = cfg->offset;
    self->_x_tbl = cfg->x_tbl;
    self->_y_tbl = cfg->y_tbl;
    self->_n_pts = cfg->n_pts;
    self->_y = 0.0f;
    self->_inited = true;

    return 0;
}

/* ── Step ─────────────────────────────────────────────────────────────── */

/**
 * @brief 前馈单步计算
 * @param self 对象
 * @param sp 设定值
 * @param dv 扰动量
 * @return 前馈输出；未初始化或非有限输入时返回 0
 */
static float algo_ffd_step_impl(algo_ffd_t* self, float sp, float dv) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    if (!algo_ffd_finite(sp))
        sp = 0.0f;
    if (!algo_ffd_finite(dv))
        dv = 0.0f;

    float out = self->_offset + self->_gain_dv * dv;

    if (self->_mode == ALGO_FFD_MODE_TABLE) {
        out += algo_ffd_interp1d(self->_x_tbl, self->_y_tbl, self->_n_pts, sp);
    } else {
        out += self->_gain_sp * sp;
    }

    if (!algo_ffd_finite(out))
        return self->_y;

    self->_y = out;
    return out;
}

/* ── Reset ────────────────────────────────────────────────────────────── */

/**
 * @brief 前馈复位
 * @param self 对象
 */
static void algo_ffd_reset_impl(algo_ffd_t* self) {
    if (self == NULL || !self->_inited)
        return;
    self->_y = 0.0f;
}

/* ── interp1d (binary search) ────────────────────────────────────────── */

/**
 * @brief 一维分段线性插值（二分查找）
 * @param x_tbl 断点数组（递增）
 * @param y_tbl 输出数组
 * @param count 断点数量
 * @param x 查询点
 * @return 插值结果；参数非法或非有限输入时返回 0
 */
static float algo_ffd_interp1d(const float* x_tbl, const float* y_tbl, uint16_t count, float x) {
    if (x_tbl == NULL || y_tbl == NULL || count < 2 || !algo_ffd_finite(x)) {
        return 0.0f;
    }

    if (x <= x_tbl[0])
        return y_tbl[0];
    if (x >= x_tbl[count - 1])
        return y_tbl[count - 1];

    uint16_t lo = 0;
    uint16_t hi = count - 1;

    while (hi - lo > 1) {
        uint16_t mid = (lo + hi) >> 1;
        if (x < x_tbl[mid]) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    float frac = (x - x_tbl[lo]) / (x_tbl[hi] - x_tbl[lo]);
    return y_tbl[lo] + frac * (y_tbl[hi] - y_tbl[lo]);
}

/* ── Constructor ──────────────────────────────────────────────────────── */

/**
 * @brief 构造前馈对象
 * @param self 对象
 */
void algo_ffd_ctor(algo_ffd_t* self) {
    if (self == NULL)
        return;

    self->init = algo_ffd_init_impl;
    self->step = algo_ffd_step_impl;
    self->reset = algo_ffd_reset_impl;
    self->_mode = ALGO_FFD_MODE_LINEAR;
    self->_gain_sp = 0.0f;
    self->_gain_dv = 0.0f;
    self->_offset = 0.0f;
    self->_x_tbl = NULL;
    self->_y_tbl = NULL;
    self->_n_pts = 0;
    self->_y = 0.0f;
    self->_inited = false;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PID + Feedforward  —  Unified Controller
 * ═══════════════════════════════════════════════════════════════════════ */

static int algo_pid_ffd_init_impl(algo_pid_ffd_t* self, const algo_pid_ffd_cfg_t* cfg);
static float algo_pid_ffd_step_impl(algo_pid_ffd_t* self, float sp, float pv, float dv);
static void algo_pid_ffd_reset_impl(algo_pid_ffd_t* self);

/**
 * @brief 初始化 PID + 前馈联合控制器
 * @param self 对象
 * @param cfg 配置
 * @return 0 成功；负数错误码
 */
static int algo_pid_ffd_init_impl(algo_pid_ffd_t* self, const algo_pid_ffd_cfg_t* cfg) {
    if (self != NULL)
        self->_inited = false;
    if (self == NULL)
        return -1;
    if (cfg == NULL)
        return -2;

    int rc = self->_ffd.init(&self->_ffd, &cfg->ffd);
    if (rc < 0)
        return rc;

    rc = self->_pid.init(&self->_pid, &cfg->pid);
    if (rc < 0)
        return rc - 100;

    self->_inited = true;
    return 0;
}

/**
 * @brief PID + 前馈联合控制器单步计算
 * @param self 对象
 * @param sp 设定值
 * @param pv 测量值
 * @param dv 扰动量
 * @return 控制输出；未初始化时返回 0
 */
static float algo_pid_ffd_step_impl(algo_pid_ffd_t* self, float sp, float pv, float dv) {
    if (self == NULL || !self->_inited)
        return 0.0f;

    float out_pid = self->_pid.step(&self->_pid, sp, pv);
    float out_ffd = self->_ffd.step(&self->_ffd, sp, dv);

    if (!algo_ffd_finite(out_pid))
        out_pid = 0.0f;
    if (!algo_ffd_finite(out_ffd))
        out_ffd = 0.0f;

    float out = out_pid + out_ffd;
    if (!algo_ffd_finite(out))
        return out_pid;
    return out;
}

/**
 * @brief PID + 前馈联合控制器复位
 * @param self 对象
 */
static void algo_pid_ffd_reset_impl(algo_pid_ffd_t* self) {
    if (self == NULL || !self->_inited)
        return;

    self->_pid.reset(&self->_pid);
    self->_ffd.reset(&self->_ffd);
}

/**
 * @brief 构造 PID + 前馈联合控制器对象
 * @param self 对象
 */
void algo_pid_ffd_ctor(algo_pid_ffd_t* self) {
    if (self == NULL)
        return;

    self->init = algo_pid_ffd_init_impl;
    self->step = algo_pid_ffd_step_impl;
    self->reset = algo_pid_ffd_reset_impl;
    self->_inited = false;

    algo_pid_ctor(&self->_pid);
    algo_ffd_ctor(&self->_ffd);
}

/**
 * @brief 获取内部 PID 对象
 * @param self 联合控制器对象
 * @return PID 对象指针；self 为 NULL 时返回 NULL
 */
algo_pid_t* algo_pid_ffd_get_pid(algo_pid_ffd_t* self) { return self ? &self->_pid : NULL; }

/**
 * @brief 获取内部前馈对象
 * @param self 联合控制器对象
 * @return 前馈对象指针；self 为 NULL 时返回 NULL
 */
algo_ffd_t* algo_pid_ffd_get_ffd(algo_pid_ffd_t* self) { return self ? &self->_ffd : NULL; }
