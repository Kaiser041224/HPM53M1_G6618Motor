/**
 * @file    app_motor_identify.h
 * @brief   电角度自校准（隔离模块：按需执行、产出数据、显式应用/持久化）
 * @author  Kaiser
 *
 * 隔离契约（方案 A：模块隔离，共享电流环）：
 *   - 只在 1kHz 任务上下文执行（app_motor_identify_run_once，空闲时为空操作）；
 *     不进 25kHz ISR，FOC 主环路不感知校准算法；
 *   - 只经 app_foc 校准端口借用电流环：begin → 强制角 i_d 激励（id_encoder 扫描）
 *     → 试用零点闭环验证（i_d 锁定 + i_q 探针）→ end（运行零点恢复）；
 *   - 只读编码器快照与 FOC 电流快照；
 *   - 结果是数据：完成后不改运行参数、不改 FOC 零点；
 *     app_motor_identify_apply() 显式生效（RAM），app_motor_identify_save() 显式落 flash；
 *   - 自带安全中止：过流（|i_dq| 持续超限）、编码器错误/跳变、FOC 状态被外部改变、超时。
 *
 * 流程时长约 19s（lock-in 0.8s + 方向 0.4s + 正/反扫各 3 电周期 × 360 步 × 8ms + 验证 0.16s）。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_MOTOR_IDENTIFY_H
#define APP_MOTOR_IDENTIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 失败原因
 */
typedef enum {
    APP_IDENTIFY_REASON_NONE = 0,    /**< 无失败 */
    APP_IDENTIFY_REASON_TIMEOUT,     /**< 超时（辨识或编排） */
    APP_IDENTIFY_REASON_DIR,         /**< 方向判定无效（转子未跟随） */
    APP_IDENTIFY_REASON_QUALITY,     /**< 质量不足 */
    APP_IDENTIFY_REASON_RATIO,       /**< 极对数/传动比校验失败 */
    APP_IDENTIFY_REASON_HYST,        /**< 正/反向零点差超限：传动回差/联轴打滑 */
    APP_IDENTIFY_REASON_NONFINITE,   /**< 非有限测量样本过多 */
    APP_IDENTIFY_REASON_VERIFY,      /**< 闭环验证失败 */
    APP_IDENTIFY_REASON_ENCODER,     /**< 编码器读失败/错误计数增长/坏帧跳变 */
    APP_IDENTIFY_REASON_FAULT,       /**< 故障模块进入 FAULT */
    APP_IDENTIFY_REASON_STATE,       /**< FOC 状态被外部改变（非 CALIB）/ 端口拒绝 */
    APP_IDENTIFY_REASON_ABORTED,     /**< 用户中止 */
    APP_IDENTIFY_REASON_OVERCURRENT, /**< 实测 |i_dq| 持续超限（校准自带保护） */
} app_identify_fail_t;

/**
 * @brief 校准结果与状态（供 Comm/Debug 层读取并显示；Control 不直接打印）
 */
typedef struct {
    bool active;              /**< 流程进行中 */
    bool done;                /**< 完成且验证通过（结果可 apply） */
    bool failed;              /**< 失败（原因见 fail_reason） */
    float offset_rad;         /**< 电角度零点 [rad]（正/反向合并圆均值） */
    float direction;          /**< 方向（+1.0 / −1.0） */
    float quality;            /**< 质量 |Σ|/N（1 = 完全一致） */
    float ratio_err;          /**< 极对数校验偏差（相对） */
    float verify_mean_deg;    /**< 验证静默段角度残差均值 [deg]（信息量） */
    float verify_max_deg;     /**< 验证静默段角度残差峰值 [deg]（判据 ≤ 90） */
    float probe_travel_rad;   /**< 探针段机械行程 [rad]（判据：×direction ≥ 0.05） */
    float verify_drift_deg_s; /**< 静默段残差漂移 [deg/s]（≈0 静止；大 = 转子被恒转矩驱动） */
    float progress;           /**< 进度 0~1 */
    float offset_fwd_rad;     /**< 正向扫描零点 [rad]（诊断） */
    float offset_rev_rad;     /**< 反向扫描零点 [rad]（诊断；与正向之差 = 回差/打滑） */
    float offset_end_rad;     /**< 反向扫描终点零点 [rad]（静止保持点；trial 候选） */
    float i_peak_a;           /**< 流程中实测 |i_dq| 峰值 [A] */
    uint32_t elapsed_ms;      /**< 流程已用时 [ms] */
    app_identify_fail_t fail_reason; /**< 失败原因 */
} app_motor_identify_result_t;

/**
 * @brief 是否处于校准流程（进行中；完成/失败/中止后为 false）
 */
bool app_motor_identify_is_active(void);

/**
 * @brief 读取校准结果/状态快照（Comm/Debug 显示用）
 * @param out 输出结果
 */
void app_motor_identify_get_result(app_motor_identify_result_t* out);

/**
 * @brief 启动校准：前置检查 + 经端口进入 FOC CALIB
 * @return 0 = 成功；-1 = 拒绝（进行中 / 故障 / FOC 非 READY/RUN / 配置非法）
 * @note 前置：FOC 已使能（foc on），转子可自由转动（空载）。
 */
int app_motor_identify_start(void);

/**
 * @brief 中止校准（用户取消）：结果标记 ABORTED，经端口结束 CALIB；非活动时无操作
 */
void app_motor_identify_abort(void);

/**
 * @brief 1kHz 推进（app_io 任务调用；空闲时为空操作，O(1)）
 */
void app_motor_identify_run_once(void);

/**
 * @brief 应用最近一次成功结果：写电机运行参数（RAM）+ 更新 FOC 运行零点
 * @return 0 = 成功；-1 = 无可应用结果（未完成/失败/进行中）或 FOC 拒绝（CALIB 中）
 * @note 不写 flash（掉电失效）；持久化见 app_motor_identify_save()。
 */
int app_motor_identify_apply(void);

/** save 返回值：存在尚未 apply 的成功校准结果（防止把旧值误当新结果落盘） */
#define APP_IDENTIFY_SAVE_PENDING (-2)

/**
 * @brief 把当前电机运行参数中的电角度零点/方向写入 flash（记录含极对数）
 * @return 0 = 成功；-1 = FOC 未关闭 / 参数非法 / 存储失败；
 *         APP_IDENTIFY_SAVE_PENDING = 最近一次成功结果尚未 apply（先 apply 再 save）
 * @note flash 操作仅允许停机窗口（FOC OFF）。
 */
int app_motor_identify_save(void);

/**
 * @brief 上电恢复：从 flash 读取零点/方向并写入电机运行参数（须在 app_foc_init 之前调用）
 * @return 0 = 已恢复；1 = 无记录（保持 YAML 默认值）；-1 = 记录非法/极对数不符（未恢复）
 */
int app_motor_identify_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MOTOR_IDENTIFY_H */
