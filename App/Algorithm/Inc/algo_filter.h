/**
 * @file    algo_filter.h
 * @brief   滤波库（滑动平均 / 一阶低通 / FIR / Biquad / 中值）
 * @author  Kaiser
 *
 * Filter Library — Moving Average / FIR / Biquad / LPF
 *
 * MATLAB integration ready. See comments for coefficient generation.
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ALGO_FILTER_H
#define ALGO_FILTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALGO_RAMFUNC
# define ALGO_RAMFUNC __attribute__((section(".fast")))
#endif

#define ALGO_PI_F 3.14159265358979323846f /**< π */

/* ── IEEE-754 finite check (bit-level, survives -ffast-math) ──────────── */

/**
 * @brief IEEE-754 有限值判断（基于位操作，-ffast-math 下仍有效）
 * @param x 待判断值
 * @return true = 有限值
 */
ALGO_RAMFUNC
static inline bool algo_flt_finite(float x) {
    union {
        float as_float;
        uint32_t as_uint32;
    } bits = {.as_float = x};
    return (bits.as_uint32 & 0x7F800000u) != 0x7F800000u;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Moving Average  —  滑动平均 (O(1) running sum, 乘法代替除法)
 *
 *  MATLAB:  y = movmean(x, N)
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct algo_ma algo_ma_t;

/**
 * @brief 滑动平均配置
 */
typedef struct {
    uint16_t window_size; /**< 窗口长度（采样点数） */
    float* buffer;        /**< 外部环形缓冲，长度 ≥ window_size */
} algo_ma_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_ma_init_fn)(algo_ma_t* self, const algo_ma_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_ma_step_fn)(algo_ma_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_ma_reset_fn)(algo_ma_t* self);

/**
 * @brief 滑动平均对象
 */
struct algo_ma {
    struct {
        algo_ma_init_fn init;   /**< 初始化 */
        algo_ma_step_fn step;   /**< 单步计算 */
        algo_ma_reset_fn reset; /**< 复位 */
    };

    float* _buf;                /**< 外部环形缓冲 */
    uint16_t _size;             /**< 窗口长度 */
    float _inv_size;            /**< 1 / 窗口长度 */
    uint16_t _idx;              /**< 环形缓冲写入位置 */
    float _sum;                 /**< 窗口内线性和 */
    bool _filled;               /**< 窗口已填满 */
    bool _inited;               /**< 初始化标志 */
};

/**
 * @brief 构造滑动平均对象（绑定方法并清零状态）
 */
void algo_ma_ctor(algo_ma_t* self);

/**
 * @brief 滑动平均快速单步（ISR 内联路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
ALGO_RAMFUNC
static inline float algo_ma_step_fast(algo_ma_t* self, float x) {
    if (!self->_inited)
        return 0.0f;

    if (!algo_flt_finite(x)) {
        return self->_filled ? (self->_sum * self->_inv_size)
                             : (self->_sum / (float)((self->_idx > 0U) ? self->_idx : 1U));
    }

    float old = self->_buf[self->_idx];
    self->_buf[self->_idx] = x;

    self->_idx++;
    if (self->_idx >= self->_size)
        self->_idx = 0;

    if (self->_filled) {
        self->_sum += x - old;
        return self->_sum * self->_inv_size;
    }

    self->_sum += x;
    if (self->_idx == 0U) {
        self->_filled = true;
    }
    return self->_sum / (float)((self->_idx > 0U) ? self->_idx : 1U);
}

/* ════════════════════════════════════════════════════════════════════════
 *  1st-Order Low-Pass  —  一阶 IIR 低通
 *
 *     alpha = sample_time_s / (sample_time_s + time_const_s)
 *     y[n]  = alpha * x[n] + (1-alpha) * y[n-1]
 *
 *  首次有效输入直接输出（priming），避免从 0 爬升。
 *
 *  MATLAB:  y = lowpass(x, fc, fs)
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct algo_lpf algo_lpf_t;

/**
 * @brief 一阶低通配置
 */
typedef struct {
    float cutoff_hz;      /**< 截止频率 [Hz] */
    float sample_rate_hz; /**< 采样率 [Hz] */
} algo_lpf_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_lpf_init_fn)(algo_lpf_t* self, const algo_lpf_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_lpf_step_fn)(algo_lpf_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_lpf_reset_fn)(algo_lpf_t* self);

/**
 * @brief 一阶低通对象
 */
struct algo_lpf {
    struct {
        algo_lpf_init_fn init;   /**< 初始化 */
        algo_lpf_step_fn step;   /**< 单步计算 */
        algo_lpf_reset_fn reset; /**< 复位 */
    };

    float _alpha;                /**< 一阶滤波系数 */
    float _y;                    /**< 上次输出 */
    bool _primed;                /**< 首次有效输入标志 */
    bool _inited;                /**< 初始化标志 */
};

/**
 * @brief 构造一阶低通对象（绑定方法并清零状态）
 */
void algo_lpf_ctor(algo_lpf_t* self);

/**
 * @brief 一阶低通快速单步（ISR 内联路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
ALGO_RAMFUNC
static inline float algo_lpf_step_fast(algo_lpf_t* self, float x) {
    if (!self->_inited)
        return 0.0f;

    if (!algo_flt_finite(x))
        return self->_y;

    if (!self->_primed) {
        self->_y = x;
        self->_primed = true;
        return x;
    }

    self->_y += self->_alpha * (x - self->_y);
    return self->_y;
}

/* ════════════════════════════════════════════════════════════════════════
 *  FIR  —  有限冲激响应
 *
 *     y[n] = Σ h[k]·x[n−k],  k = 0 … N−1
 *
 *  MATLAB:
 *     h = fir1(N-1, fc/(fs/2));                % 窗函数法
 *     h = firpm(N-1, [0 f1 f2 1], [1 1 0 0]);  % Parks-McClellan
 *     → const float fir_coeffs[N] = { … };
 *     → algo_fir_cfg_t  .coeffs = fir_coeffs
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct algo_fir algo_fir_t;

/**
 * @brief FIR 配置
 */
typedef struct {
    const float* coeffs; /**< 系数数组，长度 num_taps */
    uint16_t num_taps;   /**< 抽头数 */
    float* buffer;       /**< 外部历史缓冲，长度 ≥ num_taps */
} algo_fir_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_fir_init_fn)(algo_fir_t* self, const algo_fir_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_fir_step_fn)(algo_fir_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_fir_reset_fn)(algo_fir_t* self);

/**
 * @brief FIR 对象
 */
struct algo_fir {
    struct {
        algo_fir_init_fn init;   /**< 初始化 */
        algo_fir_step_fn step;   /**< 单步计算 */
        algo_fir_reset_fn reset; /**< 复位 */
    };

    const float* _coeffs;        /**< 系数数组 */
    float* _buf;                 /**< 历史缓冲 */
    uint16_t _taps;              /**< 抽头数 */
    uint16_t _idx;               /**< 写入位置 */
    float _y;                    /**< 上次输出 */
    bool _inited;                /**< 初始化标志 */
};

/**
 * @brief 构造 FIR 对象（绑定方法并清零状态）
 */
void algo_fir_ctor(algo_fir_t* self);

/* ════════════════════════════════════════════════════════════════════════
 *  Biquad  —  二阶 IIR (Direct Form II Transposed)
 *
 *     y[n]   = b0·x[n] + z1[n−1]
 *     z1[n]  = b1·x[n] − a1·y[n] + z2[n−1]
 *     z2[n]  = b2·x[n] − a2·y[n]
 *
 *  假定 a0 已归一化为 1（MATLAB 输出 [b,a] 后除以 a(1)）。
 *
 *  MATLAB:
 *     [b,a] = butter(2, fc/(fs/2));
 *     b = b / a(1);  a = a / a(1);
 *     → algo_biquad_coeffs_t = {b(1),b(2),b(3), a(2),a(3)}
 * ════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Biquad 系数（a0 = 1）
 */
typedef struct {
    float b0, b1, b2; /**< 分子系数 */
    float a1, a2;     /**< 分母系数 */
} algo_biquad_coeffs_t;

typedef struct algo_biquad algo_biquad_t;

/**
 * @brief Biquad 配置
 */
typedef struct {
    algo_biquad_coeffs_t coeffs; /**< 系数 */
} algo_biquad_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_biquad_init_fn)(algo_biquad_t* self, const algo_biquad_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_biquad_step_fn)(algo_biquad_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_biquad_reset_fn)(algo_biquad_t* self);

/**
 * @brief Biquad 对象
 */
struct algo_biquad {
    struct {
        algo_biquad_init_fn init;   /**< 初始化 */
        algo_biquad_step_fn step;   /**< 单步计算 */
        algo_biquad_reset_fn reset; /**< 复位 */
    };

    float _b0, _b1, _b2;            /**< 分子系数 */
    float _a1, _a2;                 /**< 分母系数 */
    float _z1, _z2;                 /**< 状态变量 */
    float _y;                       /**< 上次输出 */
    bool _inited;                   /**< 初始化标志 */
};

/**
 * @brief 构造 Biquad 对象（绑定方法并清零状态）
 */
void algo_biquad_ctor(algo_biquad_t* self);

/**
 * @brief Biquad 快速单步（ISR 内联路径）
 * @param self 对象
 * @param x 输入样本
 * @return 滤波输出
 */
ALGO_RAMFUNC
static inline float algo_biquad_step_fast(algo_biquad_t* self, float x) {
    if (!self->_inited)
        return 0.0f;

    if (!algo_flt_finite(x))
        return self->_y;

    float out = self->_b0 * x + self->_z1;
    self->_z1 = self->_b1 * x - self->_a1 * out + self->_z2;
    self->_z2 = self->_b2 * x - self->_a2 * out;
    self->_y = out;
    return out;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Median  —  滑动窗中值滤波
 *
 *     y[n] = median{ x[n−k] },  k = 0 … N−1
 *
 *  对脉冲噪声（尖峰、毛刺）鲁棒性优于均值滤波。
 *  使用插入排序 O(N²·per step)，适用于小窗口 (N ≤ 31)。
 *
 *  MATLAB:  y = medfilt1(x, N)
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct algo_med algo_med_t;

/**
 * @brief 中值滤波配置
 */
typedef struct {
    uint16_t window_size; /**< 窗口长度（≥3） */
    float* buffer;        /**< 外部环形缓冲，长度 ≥ window_size */
    float* sort_buf;      /**< 外部排序缓冲，长度 ≥ window_size */
} algo_med_cfg_t;

/**
 * @brief 初始化
 */
typedef int (*algo_med_init_fn)(algo_med_t* self, const algo_med_cfg_t* cfg);
/**
 * @brief 单步计算
 */
typedef float (*algo_med_step_fn)(algo_med_t* self, float x);
/**
 * @brief 复位
 */
typedef void (*algo_med_reset_fn)(algo_med_t* self);

/**
 * @brief 中值滤波对象
 */
struct algo_med {
    struct {
        algo_med_init_fn init;   /**< 初始化 */
        algo_med_step_fn step;   /**< 单步计算 */
        algo_med_reset_fn reset; /**< 复位 */
    };

    float* _buf;                 /**< 环形缓冲 */
    float* _sort;                /**< 排序缓冲 */
    uint16_t _size;              /**< 窗口长度 */
    uint16_t _idx;               /**< 写入位置 */
    uint16_t _count;             /**< 已填充样本数 */
    float _y;                    /**< 上次输出 */
    bool _inited;                /**< 初始化标志 */
};

/**
 * @brief 构造中值滤波对象（绑定方法并清零状态）
 */
void algo_med_ctor(algo_med_t* self);

#ifdef __cplusplus
}
#endif

#endif /* ALGO_FILTER_H */
