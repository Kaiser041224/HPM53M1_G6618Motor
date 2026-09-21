/**
 * @file    id_encoder.h
 * @brief   电角度辨识（lock-in + 扫描 + sin/cos 累加）— 纯状态机，零硬件依赖
 * @author  Kaiser
 *
 * 阶段（spec §5.2）：IDLE → LOCK_IN → DIR → SWEEP_FWD → SWEEP_REV → COMPUTE → DONE/FAILED
 * 输出：电角度零点 offset_rad、方向 direction、质量 quality、极对数校验 mech_ratio_err
 *
 * 说明：
 *   - 终止态（DONE/FAILED）输出 i_d_ref/i_q_ref = 0（消费方无需额外断电）；
 *   - 机械位移仅累计正向扫描（排除 DIR→FWD 回摆瞬态），期望值按 (steps−1)/steps 补偿；
 *   - 总超时（V1）；非有限测量样本 > 3 → FAILED(NONFINITE)；
 *   - 失败原因经 fail_reason 区分（超时/方向/质量/极对数/非有限）。
 *
 * 符号约定（spec §5.1）：θe_true = p·dir·θm − offset；辨识期间 θe_true = θapplied
 *   → δ = θapplied − p·dir·θm = −offset → offset = −atan2(Σsin δ, Σcos δ)
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ID_ENCODER_H
#define ID_ENCODER_H

#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 辨识配置
 */
typedef struct {
    uint8_t pole_pairs;   /**< 极对数 */
    float i_cal_a;        /**< 辨识电流（d 轴）[A] */
    float lockin_ms;      /**< lock-in 时长 [ms] */
    float dir_ms;         /**< 方向判定步时长 [ms] */
    float dir_step_rad;   /**< 方向判定电角度步进 [rad] */
    uint16_t sweep_steps; /**< 单次扫描步数（≥8） */
    float sweep_step_ms;  /**< 每步驻留时长 [ms] */
    float quality_min;    /**< 质量下限（低于则 FAILED） */
    float ratio_tol;      /**< 极对数校验容差（相对） */
    float timeout_ms;     /**< 总超时 [ms] */
} id_encoder_cfg_t;

/**
 * @brief 辨识阶段
 */
typedef enum {
    ID_ENCODER_PHASE_IDLE = 0,   /**< 空闲 */
    ID_ENCODER_PHASE_LOCK_IN,    /**< 锁定对齐 */
    ID_ENCODER_PHASE_DIR,        /**< 方向判定 */
    ID_ENCODER_PHASE_SWEEP_FWD,  /**< 正向扫描 */
    ID_ENCODER_PHASE_SWEEP_REV,  /**< 反向扫描 */
    ID_ENCODER_PHASE_COMPUTE,    /**< 解算 */
    ID_ENCODER_PHASE_DONE,       /**< 完成 */
    ID_ENCODER_PHASE_FAILED,     /**< 失败 */
} id_encoder_phase_t;

/**
 * @brief 单步输入（测量回喂）
 */
typedef struct {
    float theta_m_raw_rad; /**< 机械角（未加软件零点）[rad] */
    float i_d_a, i_q_a;    /**< 电流反馈 [A] */
    float v_bus_v;         /**< 母线电压 [V] */
    float dt_s;            /**< 本步间隔 [s] */
} id_encoder_in_t;

/**
 * @brief 失败原因
 */
typedef enum {
    ID_ENCODER_FAIL_NONE = 0,  /**< 无失败 */
    ID_ENCODER_FAIL_TIMEOUT,   /**< 总超时 */
    ID_ENCODER_FAIL_DIR,       /**< 方向判定无效（转子未跟随） */
    ID_ENCODER_FAIL_QUALITY,   /**< 质量不足 */
    ID_ENCODER_FAIL_RATIO,     /**< 极对数/传动比校验失败 */
    ID_ENCODER_FAIL_NONFINITE, /**< 非有限测量样本过多 */
    ID_ENCODER_FAIL_CONFIG,    /**< 配置非法 */
} id_encoder_fail_t;

/**
 * @brief 单步输出（激励请求 + 结果）
 */
typedef struct {
    float theta_e_cmd;        /**< 强制电角度 [rad] */
    float i_d_ref;            /**< d 轴电流给定 [A]（终止态为 0） */
    float i_q_ref;            /**< q 轴电流给定 [A]（终止态为 0） */
    id_encoder_phase_t phase; /**< 当前阶段 */
    float progress;           /**< 进度 0~1 */
    float offset_rad;         /**< 结果：电角度零点 [rad] */
    float direction;          /**< 结果：方向（+1.0 / −1.0） */
    float quality;            /**< 结果：质量 |Σ|/N */
    float mech_ratio_err;     /**< 结果：极对数校验偏差（相对） */
    bool done;                /**< 完成 */
    bool failed;              /**< 失败 */
    id_encoder_fail_t fail_reason; /**< 失败原因 */
} id_encoder_out_t;

typedef struct id_encoder id_encoder_t;

/**
 * @brief 初始化
 * @return 0 = 成功；-1 = 参数非法
 */
typedef int (*id_encoder_init_fn)(id_encoder_t* self, const id_encoder_cfg_t* cfg);
/**
 * @brief 单步（25kHz；内部按时长推进阶段）
 */
typedef void (*id_encoder_step_fn)(id_encoder_t* self, const id_encoder_in_t* in,
                                   id_encoder_out_t* out);
/**
 * @brief 复位（回到 LOCK_IN，清除累计）
 */
typedef void (*id_encoder_reset_fn)(id_encoder_t* self);

/**
 * @brief 辨识对象
 */
struct id_encoder {
    struct {
        id_encoder_init_fn init;   /**< 初始化 */
        id_encoder_step_fn step;   /**< 单步 */
        id_encoder_reset_fn reset; /**< 复位 */
    };

    id_encoder_cfg_t _cfg;     /**< 配置 */
    id_encoder_phase_t _phase; /**< 当前阶段 */
    float _t_ms;               /**< 当前阶段计时 [ms] */
    float _elapsed_ms;         /**< 总计时 [ms] */
    float _theta_cmd;          /**< 当前强制角 [rad] */
    uint16_t _step_idx;        /**< 扫描步索引 */
    float _s_sum, _c_sum;      /**< sin/cos 累加 */
    uint32_t _acc_n;           /**< 累加点数 */
    float _theta_m_start;      /**< 方向判定起点机械角 [rad] */
    float _mech_travel;        /**< 扫描累计机械位移（wrap-safe）[rad] */
    float _theta_m_prev;       /**< 上拍机械角 [rad] */
    bool _theta_m_prev_valid;  /**< 上拍机械角有效 */
    float _offset_rad;         /**< 结果：零点 [rad] */
    float _direction;          /**< 结果：方向 */
    float _quality;            /**< 结果：质量 */
    float _mech_ratio_err;     /**< 结果：极对数偏差 */
    id_encoder_fail_t _fail;   /**< 失败原因 */
    uint8_t _bad_samples;      /**< 非有限测量样本计数 */
    bool _inited;              /**< 初始化标志 */
};

/**
 * @brief 构造对象（绑定方法并清零状态）
 */
void id_encoder_ctor(id_encoder_t* self);

#ifdef __cplusplus
}
#endif

#endif /* ID_ENCODER_H */
