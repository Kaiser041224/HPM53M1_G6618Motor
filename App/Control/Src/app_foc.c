/**
 * @file    app_foc.c
 * @brief   FOC 编排实现
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_foc.h"

#include "app_3phase_inverter.h"
#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_protect_policy.h"
#include "app_ramfunc.h"
#include "app_software_params.h"
#include "foc_angle.h"
#include "foc_math.h"
#include "intf_clock.h"
#include "intf_sys.h"

#define APP_FOC_SPEED_LPF_HZ (100.0f) /**< ωe 估计低通截止 [Hz] */
/* 限速滞环：超限切断，回落至 85% 才恢复（ωe 噪声/回摆不再造成转矩断续） */
#define APP_FOC_SPEED_LIMIT_RESTORE (0.85f)

/* 编码器同步采样节拍与新鲜度：与电流同拍（25kHz → 40µs，同一 PWM1 CMP10 触发）。
 * 新鲜度上限 = 采样周期 + 40µs 余量 = 80µs：最多容忍一拍丢样复用保持角。 */
#define APP_FOC_ENC_PERIOD_US  (40U)
#define APP_FOC_ENC_MARGIN_US  (40U)
#define APP_FOC_ENC_MAX_AGE_US (APP_FOC_ENC_PERIOD_US + APP_FOC_ENC_MARGIN_US)
/* 连续失败/跳变阈值：达到即判换相角不可信（与 algo fail_limit 默认一致） */
#define APP_FOC_ENC_FAIL_TRIP (3U)
/* 时序类编码器故障确认（坏帧拒绝发布 → age 越限到 2 采样周期，单拍不判死） */
#define APP_FOC_ENC_DEG_TRIP_STREAK (32U) /**< 连续 32 拍（25kHz ≈ 1.3ms）不可用才停机 */
#define APP_FOC_OVR_TRIP_STREAK     (32U) /**< 连续 32 拍超预算才停用快路径（真失控/风暴） */

static app_foc_state_t s_state;
static foc_angle_t s_angle;
static app_foc_angle_source_t s_angle_src;
static float s_forced_theta;
static float s_i_d_ref;
static float s_i_q_ref_cmd;               /**< 操作员 q 轴给定 [A]（未限速） */
static app_foc_current_source_t s_current_source;
static app_foc_current_request_t s_current_request;
static uint32_t s_current_epoch;
static uint32_t s_current_request_cycle;
static bool s_current_request_expired;    /**< 过期锁存，直到新请求/权限撤销，防时间回绕复活 */
static app_foc_execution_t s_execution;

/** @brief 调用方已持有临界区，撤销旧正常请求 */
static void app_foc_revoke_current(void) {
    ++s_current_epoch;
    s_current_request = (app_foc_current_request_t){0};
    s_current_request_expired = false;
}
static float s_i_q_ref;                  /**< 生效 q 轴给定 [A]（含转矩模式限速） */
static uint32_t s_rotor_seq_last;        /**< 上一接受样本序号（复用检测） */
static bool s_rotor_seq_primed;          /**< 序号基准已建立 */
static uint32_t s_enc_ts_last;           /**< 上一接受样本时间戳 [cycle]（速度 dt 基准） */
static bool s_enc_ts_valid;              /**< 时间戳基准已建立 */
static float s_theta_held;               /**< 复用样本时的电角度 [rad] */
static float s_omega_held;               /**< 复用样本时的 ωe [rad/s] */
static bool s_theta_held_valid;          /**< θe/ωe 保持值已建立 */
static bool s_angle_ready;               /**< 角度链初始化成功 */
static bool s_initialized;               /**< app_foc_init 已执行 */
static uint32_t s_last_cycle;            /**< 主循环上一拍时刻 [cycle]（实测 dt 用） */
static bool s_dt_valid;                  /**< 主循环 dt 基准已建立 */
static float s_dt_s;                     /**< 主循环实测间隔 [s]（0 = 首拍/异常） */
static bool s_speed_limited;             /**< 限速滞环状态（避免阈值附近转矩断续） */
static volatile bool s_isr_disabled;     /**< ISR 快路径超预算后停用（防止饿死主循环） */
static volatile bool s_emergency_active; /**< ISR 紧急关桥已执行（避免重复开销） */
static volatile bool s_isr_inhibited;    /**< ISR 输出抑制（故障锁存；仅 enable 清除） */
static uint32_t s_isr_last_cycle;        /**< ADC ISR 上一拍时刻 [cycle]（vtest 真实 dt） */
static bool s_isr_dt_valid;              /**< ADC ISR dt 基准已建立 */
static uint8_t s_enc_degraded_streak;    /**< 编码器时序类不可用连续拍数（streak 确认） */
static uint8_t s_overrun_streak;         /**< ISR 超预算连续拍数（streak 确认） */
static bool s_stop_pending;
static uint16_t s_stop_ticks;
static float s_stop_initial_id, s_stop_initial_iq;

/* ISR 用时钟换算常量（app_foc_init 预计算：时钟树在调度器前已定型，热路径免查询/免除法） */
static uint32_t s_cycles_per_us;      /**< CPU cycle / µs */
static float s_sec_per_cycle;         /**< 1 / CPU 频率 [s/cycle] */
static uint32_t s_enc_max_age_cycles; /**< 编码器样本新鲜度上限 [cycle] */
static uint32_t s_isr_budget_cycles;  /**< ISR 单拍预算 [cycle] */

/* 运行期角度链零点镜像（不含 CALIB 试用值）与校准期间的备份 */
static float s_angle_offset_rad; /**< 当前生效电角度零点 [rad] */
static float s_angle_direction;  /**< 当前生效方向（±1） */

/** 外部（Debug）紧急停机请求邮箱（Control 持有指针；ISR 只读，绝不阻塞） */
static volatile uint32_t* s_estop_request;

/** ISR 单拍预算 [µs]（ISR 周期 40µs；超限即停用快路径并安全关桥） */
#define APP_FOC_ISR_BUDGET_US (25U)

/* Ozone 观测：ISR 超预算次数 */
volatile uint32_t g_foc_isr_overruns __attribute__((section(".noncacheable.bss")));

/* Ozone 观测：FOC 单拍耗时 [cycle]（.noncacheable.bss，调试器直读） */
volatile uint32_t g_foc_loop_cycles __attribute__((section(".noncacheable.bss")));
/* Ozone 观测：主循环调用间隔 [µs]（25kHz 标称；抖动/丢拍时明显大于 40） */
volatile uint32_t g_foc_loop_dt_us __attribute__((section(".noncacheable.bss")));
/* Ozone 观测：25kHz ISR 单拍耗时 [cycle] 与历史最大 */
volatile uint32_t g_foc_isr_cycles __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_foc_isr_cycles_max __attribute__((section(".noncacheable.bss")));
/* Ozone 观测：ISR 侧编码器快照年龄 [cycle] / [µs] 与接受序号 */
volatile uint32_t g_foc_enc_age_cycles __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_foc_enc_age_us __attribute__((section(".noncacheable.bss")));
volatile uint32_t g_foc_enc_seq __attribute__((section(".noncacheable.bss")));
/* ISR→主循环 故障请求（ISR 只置位；主循环负责状态迁移） */
volatile uint32_t g_foc_fault_request __attribute__((section(".noncacheable.bss")));

/**
 * @brief 实测本拍与上一拍的调用间隔 [s]
 * @note 主循环节拍存在抖动/丢拍（USB/终端/打印同循环），固定 1/25kHz 换算 ωe
 *       会产生数倍尖峰；首拍返回 0（角度链保持 ωe 不更新）
 */
static float app_foc_measure_dt(void) {
    uint32_t now = intf_clock_get_cycle();
    float dt = 0.0f;

    if (s_dt_valid) {
        dt = (float)(now - s_last_cycle) / (float)intf_clock_get_cpu_freq();
    }
    s_last_cycle = now;
    s_dt_valid = true;
    return dt;
}

/**
 * @brief 电角度链初始化（参数来源 motor 域）
 * @return 0 = 成功；-1 = 参数非法（角度链不可用，enable 将拒绝）
 */
static int app_foc_angle_init(void) {
    const app_motor_params_t* motor = app_motor_params_current();
    foc_angle_cfg_t cfg;

    cfg.pole_pairs = motor->pole_pairs;
    cfg.direction = motor->encoder.direction;
    cfg.offset_rad = motor->encoder.electrical_offset_rad;
    s_angle_offset_rad = cfg.offset_rad;
    s_angle_direction = cfg.direction;
    cfg.speed_lpf_hz = APP_FOC_SPEED_LPF_HZ;
    cfg.sample_time_s = 1.0f / (float)app_hardware_params_current()->inverter.pwm_freq_hz;

    foc_angle_ctor(&s_angle);
    return s_angle.init(&s_angle, &cfg);
}

void app_foc_init(void) {
    uint32_t cpu_hz = intf_clock_get_cpu_freq();

    foc_math_init(); /* sincos 查表（用 libm 一次） */

    s_cycles_per_us = (cpu_hz / 1000000U != 0U) ? (cpu_hz / 1000000U) : 1U;
    s_sec_per_cycle = (cpu_hz != 0U) ? (1.0f / (float)cpu_hz) : 0.0f;
    s_enc_max_age_cycles = s_cycles_per_us * APP_FOC_ENC_MAX_AGE_US;
    s_isr_budget_cycles = s_cycles_per_us * APP_FOC_ISR_BUDGET_US;

    s_state = APP_FOC_STATE_OFF;
    s_current_source = APP_FOC_CURRENT_SOURCE_DIRECT;
    app_foc_revoke_current();
    s_execution = (app_foc_execution_t){0};
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    s_forced_theta = 0.0f;
    s_i_d_ref = 0.0f;
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    s_angle_ready = (app_foc_angle_init() == 0);
    app_foc_current_init();
    s_initialized = true;

    /* 电流环挂到 ADC0 PMT 完成中断（固定 25kHz、采样到输出延迟最小） */
    app_adc_register_current_hook(app_foc_isr_step);
}

void app_foc_register_estop_request(volatile uint32_t* req) {
    /* Control 持有指针；ISR 每拍只读。Debug 层可在 Ozone 写 *req=1 触发紧急停机，
     * 无需主循环参与。req 生命周期须覆盖运行期（通常为 Debug 静态结构）。 */
    s_estop_request = req;
}

/**
 * @brief 读取转子机械角（未加软件零点）[rad]；含采样停摆检测
 * @return true = 有效（本拍采样序号已推进）
 * @note 序号未推进（采样停摆）→ 视为无效，调用方应输出零矢量
 */
static bool app_foc_read_rotor_rad(float* theta_m_rad) {
    app_encoder_rotor_snapshot_t enc;
    uint32_t max_age = s_enc_max_age_cycles; /* app_foc_init 预计算 */

    /* 主循环观测：读快照（8 次有界重试），仅在新样本/未陈旧时刷新角度 */
    if ((app_encoder_get_rotor_snapshot(&enc) != 0) || !enc.valid) {
        return false;
    }
    if ((max_age != 0U) && (enc.age_cycles > max_age)) {
        return false; /* 样本陈旧 */
    }
    if (s_rotor_seq_primed && (enc.seq == s_rotor_seq_last)) {
        return false; /* 无新样本（主循环观测无需重复刷新） */
    }
    s_rotor_seq_last = enc.seq;
    s_rotor_seq_primed = true;
    *theta_m_rad = enc.rad;
    return true;
}

/**
 * @brief 25kHz 单拍主体（由 app_foc_run_once 计时包裹）
 */
static void app_foc_run_body(void) {
    float omega_e = 0.0f;

    if (!s_initialized) {
        return;
    }

    /* 故障门控：仅 FAULT 状态（或 ISR 紧急请求）才切 FAULT；WARNING 是去抖中间态，
     * 不应停机（WARNING 期间 ISR 仍按正常电流环运行）。 */
    if (s_state == APP_FOC_STATE_FAULT) {
        return;
    }
    if (APP_PROTECT_ACTION_EN && (s_state != APP_FOC_STATE_OFF)
        && ((app_fault_get_state() == APP_FAULT_STATE_FAULT) || (g_foc_fault_request != 0U))) {
        app_foc_current_zero_vector();
        app_3phase_inverter_emergency_stop(); /* 幂等：ISR 侧可能已执行 */
        s_state = APP_FOC_STATE_FAULT;        /* 原误用 APP_FAULT_STATE_FAULT（=3=CALIB） */
        return;
    }
    /* 快速过流跳闸（电流环内置）：零矢量已由保护路径输出，此处关桥并锁存 FAULT */
    if (APP_PROTECT_ACTION_EN && (s_state != APP_FOC_STATE_OFF) && app_foc_current_is_tripped()) {
        app_3phase_inverter_emergency_stop();
        s_state = APP_FOC_STATE_FAULT;
        return;
    }

    if (s_state == APP_FOC_STATE_OFF) {
        /* OFF：仅刷新观测（角度 + 实测电流；不写桥、不跑电流环）。
         * 桥已关闭 → 实测电流应为 0（±噪声）；快照同步刷新，避免"OFF 仍有电流"的误读。 */
        app_analog_values_t values;
        float theta_m;

        if (s_angle_ready && app_foc_read_rotor_rad(&theta_m)) {
            float omega_off = 0.0f;
            float theta_off = s_angle.step(&s_angle, theta_m, s_dt_s, &omega_off);

            g_foc_current_snapshot.theta_e_rad = theta_off;
            g_foc_current_snapshot.omega_e_rad_s = omega_off;
            if (app_analog_signal_read_all(&values)) {
                float i_alpha, i_beta, s, c;

                foc_clarke(values.i_u_a, values.i_v_a, values.i_w_a, &i_alpha, &i_beta);
                foc_sincos(theta_off, &s, &c);
                foc_park_sc(
                    i_alpha, i_beta, s, c, &g_foc_current_snapshot.i_d_a,
                    &g_foc_current_snapshot.i_q_a);
                g_foc_current_snapshot.v_bus_v = values.v_bus_v;
            }
        }
        return;
    }

    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)
        && (s_state != APP_FOC_STATE_CALIB)) {
        return;
    }

    /* 开环电压诊断（vtest）：在 ADC0 ISR 内以本拍新鲜电流/母线 + 真实 dt 执行
     * （单一实时输出所有者：主循环不再直接写桥）。此处仅保留状态编排。 */

    /* 校准（CALIB）：由隔离的 app_motor_identify 经校准端口在 1kHz 任务中驱动；
     * 本函数不感知校准算法（无调用点）。 */

    /* 角度/电流环已迁至 app_foc_isr_step（ADC 完成回调，固定 25kHz）；
     * 此处只做状态机、限幅与激励编排。ωe 取 ISR 侧快照用于限速判据。 */
    omega_e = g_foc_current_snapshot.omega_e_rad_s;

    /* 转矩模式限速（保护，不锁存）：|ωe| 超限 → 生效给定置零；带 15% 滞环恢复
     * （无滞环时 ωe 噪声/回摆会在阈值附近反复切断 → 机械顿挫、电流冲击）。
     * CALIB 由辨识模块直接给激励（calib_set_excitation），不受限速影响。
     * 注：不再做"电压饱和 → 给定置零"的 bang-bang（与 PI 抗饱和语义冲突且造成
     * 转矩断续）；饱和信息由快照 saturated / v_scale 观测，必要时上层处理。 */
    if (s_state != APP_FOC_STATE_CALIB) {
        float speed_max = app_software_params_current()->control.limits.speed_max_rad_s;

        if (s_current_source == APP_FOC_CURRENT_SOURCE_DIRECT || s_stop_pending)
            s_i_q_ref = s_i_q_ref_cmd;
        if (foc_finite(speed_max) && (speed_max > 0.0f)) {
            if (s_speed_limited) {
                if (fabsf(omega_e) < (speed_max * APP_FOC_SPEED_LIMIT_RESTORE)) {
                    s_speed_limited = false;
                }
            } else if (fabsf(omega_e) > speed_max) {
                s_speed_limited = true;
            }
            if (s_speed_limited) {
                s_i_q_ref = 0.0f;
            }
        } else {
            s_speed_limited = false;
        }
    }

    if ((s_state == APP_FOC_STATE_READY) && ((s_i_d_ref != 0.0f) || (s_i_q_ref_cmd != 0.0f))) {
        s_state = APP_FOC_STATE_RUN;
    }
}

/**
 * @brief ISR 紧急停机：独立于主循环关桥，并置故障请求供主循环迁移状态
 */
FOC_ATTR_RAMFUNC
static void app_foc_isr_emergency(void) {
    if (!s_emergency_active) {
        s_emergency_active = true;
        s_isr_inhibited = true;               /* 锁存：后续拍不再输出 */
        if (s_estop_request != NULL) {
            *s_estop_request = 1U;            /* 回写外部请求位（Debug 可观测；需显式清零） */
        }
        app_3phase_inverter_emergency_stop(); /* 关桥 + 强制低 + 关 12V；幂等 */
    }
    g_foc_fault_request = 1U;
}

/**
 * @brief FOC 快速路径（ADC0 PMT 完成中断内调用）：编码器同步采样 + 角度链 + 电流环
 * @note 固定 25kHz。PWM1 CMP10 同一触发点同时启动三相电流 ADC（PMT）与转子 SPI 帧（HDMA 写
 *       SPI3 CMD），本 ISR 入口时两者均已完成：电流从 PMT 结果取，角度从 SPI RX FIFO 取，
 *       无启动开销、正常无忙等。编码器采样每拍执行（含 OFF 态），保证主循环快照新鲜。
 *       所有退出路径（正常/故障/无效输入）在单一出口统一计时与预算检查。
 */
FOC_ATTR_RAMFUNC
void app_foc_isr_step(void) {
    uint32_t t_isr = intf_clock_get_cycle();
    app_encoder_rotor_snapshot_t enc;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    float frame_dt_s = 0.0f;
    float isr_dt_s = 0.0f;
    float i_u = 0.0f;
    float i_v = 0.0f;
    float i_w = 0.0f;
    float v_bus;
    uint16_t raw;
    bool fault = false;
    bool degraded = false;
    bool executed = false;
    bool request_expired = false;

    /* 0) 编码器同步采样（唯一运行期 SPI3 读点；每拍执行，与 FOC 状态无关） */
    (void)app_encoder_sync_sample(t_isr, &enc);

    if (!s_initialized || s_isr_disabled) {
        return; /* 已停用：无输出，无需计时 */
    }

    /* ADC ISR 实测间隔 [s]：vtest 时长按此推进（与编码器样本 dt 无关） */
    if (s_isr_dt_valid) {
        isr_dt_s = (float)(t_isr - s_isr_last_cycle) * s_sec_per_cycle;
        if (!(isr_dt_s > 0.0f) || (isr_dt_s > 5.0e-3f)) {
            isr_dt_s = 0.0f; /* 异常/首拍：vtest 退回标称周期 */
        }
    }
    s_isr_last_cycle = t_isr;
    s_isr_dt_valid = true;

    /* ISR 内即时故障门控：故障模块 FAULT / 外部 estop / 已锁存抑制 → 关桥并锁存 */
    if (s_isr_inhibited || ((s_estop_request != NULL) && (*s_estop_request != 0U))
        || (app_fault_get_state() == APP_FAULT_STATE_FAULT)) {
        if (!s_isr_inhibited) {
            app_foc_current_protect();
        }
        app_foc_isr_emergency();
        s_isr_inhibited = true;
        goto isr_exit;
    }

    if ((s_state == APP_FOC_STATE_OFF) || (s_state == APP_FOC_STATE_FAULT)) {
        goto isr_exit; /* 桥已关断：零矢量由关闭路径保证 */
    }

    /* 角度：强制角（辨识/开环）或本拍同步采样（与电流同一触发点） */
    if (s_angle_src == APP_FOC_ANGLE_FORCED) {
        theta_e = foc_wrap_2pi(s_forced_theta);
    } else {
        bool is_new;

        g_foc_enc_age_cycles = enc.age_cycles;
        g_foc_enc_age_us = enc.age_cycles / s_cycles_per_us; /* 32 位硬件除法（非 __udivdi3） */
        g_foc_enc_seq = enc.seq;

        /* age 守卫：同 ISR 采样时恒为 0（防御性保留——采样点若移出本 ISR 即生效） */
        if (enc.valid && (enc.consecutive_fail < APP_FOC_ENC_FAIL_TRIP)
            && (enc.age_cycles <= s_enc_max_age_cycles)) {
            s_enc_degraded_streak = 0U;
            is_new = !s_rotor_seq_primed || (enc.seq != s_rotor_seq_last);
            if (is_new) {
                if (s_enc_ts_valid) {
                    /* 速度 dt 用样本时间戳差（跳变保持/丢样后仍正确） */
                    frame_dt_s = (float)(enc.timestamp_cycles - s_enc_ts_last) * s_sec_per_cycle;
                }
                s_enc_ts_last = enc.timestamp_cycles;
                s_enc_ts_valid = true;
                s_rotor_seq_last = enc.seq;
                s_rotor_seq_primed = true;

                theta_e = s_angle.step(&s_angle, enc.rad, frame_dt_s, &omega_e);
                s_theta_held = theta_e;
                s_omega_held = omega_e;
                s_theta_held_valid = true;
            } else if (s_theta_held_valid) {
                theta_e = s_theta_held; /* 跳变保持拍：复用上拍角度，不重复 step */
                omega_e = s_omega_held;
            } else {
                fault = true;           /* 尚无可用角度 */
            }
        } else if (s_theta_held_valid) {
            /* 时序/质量类不可用（坏帧丢样、超龄、连续失败）：复用保持角继续出矢量
             * （40µs 级误差可忽略）；连续 ENC_DEG_TRIP_STREAK 拍判定"真失效"事件。
             * M1（动作全关）：判定只计数、不升级 fault —— 控制输出保持连续。 */
            theta_e = s_theta_held;
            omega_e = s_omega_held;
            s_enc_degraded_streak++;
            if (s_enc_degraded_streak == APP_FOC_ENC_DEG_TRIP_STREAK) {
                app_foc_current_protect_reason(APP_FOC_PROT_ENC); /* 判定事件：计数一次 */
            }
            if (APP_PROTECT_ACTION_EN && (s_enc_degraded_streak >= APP_FOC_ENC_DEG_TRIP_STREAK)) {
                fault = true;                                     /* 动作开：确认真失效 → 停机 */
            }
        } else {
            /* 尚无可用角度（冷启动首拍/坏帧窗口）：本拍零矢量等待，不停机 */
            s_enc_degraded_streak++;
            degraded = true;
            if (s_enc_degraded_streak == APP_FOC_ENC_DEG_TRIP_STREAK) {
                app_foc_current_protect_reason(APP_FOC_PROT_ENC);
            }
            if (APP_PROTECT_ACTION_EN && (s_enc_degraded_streak >= APP_FOC_ENC_DEG_TRIP_STREAK)) {
                fault = true;
            }
        }
    }

    /* 本拍新鲜电流（原始码 → A，无滤波） */
    if (!fault) {
        if (!app_adc_get_raw(ADC_CH_I_U, &raw)) {
            app_foc_current_protect_reason(APP_FOC_PROT_READ);
            fault = true;
        } else {
            i_u = app_analog_signal_convert_raw(ADC_CH_I_U, raw);
        }
    }
    if (!fault) {
        if (!app_adc_get_raw(ADC_CH_I_V, &raw)) {
            app_foc_current_protect_reason(APP_FOC_PROT_READ);
            fault = true;
        } else {
            i_v = app_analog_signal_convert_raw(ADC_CH_I_V, raw);
        }
    }
    if (!fault) {
        if (!app_adc_get_raw(ADC_CH_I_W, &raw)) {
            app_foc_current_protect_reason(APP_FOC_PROT_READ);
            fault = true;
        } else {
            i_w = app_analog_signal_convert_raw(ADC_CH_I_W, raw);
        }
    }

    /* 母线电压：ADC1 慢速通道（1kHz 更新）；非法/过低由 run_fresh/vtest 保护 */
    v_bus = app_analog_signal_read(ADC_CH_V_VBUS);

    if (fault) {
        app_foc_current_protect();
        if (APP_PROTECT_ACTION_EN) {
            app_foc_isr_emergency(); /* M1：只判断不动作（本拍零矢量回退，不停机） */
        }
    } else if (degraded) {
        app_foc_current_protect();   /* 本拍零矢量（无角度）；下拍重试，不停机 */
    } else {
        if (s_current_source == APP_FOC_CURRENT_SOURCE_MOTION && s_state != APP_FOC_STATE_CALIB
            && !s_stop_pending && !app_foc_current_vtest_active()) {
            if (s_current_request.valid
                && (uint32_t)(t_isr - s_current_request_cycle) > s_cycles_per_us * 500U)
                s_current_request_expired = true;
            request_expired = s_current_request_expired;
            s_i_d_ref =
                s_current_request.valid && !request_expired ? s_current_request.id_ref_a : 0.0f;
            s_i_q_ref = s_current_request.valid && !request_expired && !s_speed_limited
                          ? s_current_request.iq_ref_a
                          : 0.0f;
        }
        if (app_foc_current_vtest_active()) {
            /* vtest 独占执行（ADC ISR，本拍新鲜采样 + ADC 实测 dt） */
            (void)app_foc_current_vtest_step_fresh(i_u, i_v, i_w, v_bus, isr_dt_s);
        } else {
            executed = app_foc_current_run_fresh(
                           theta_e, omega_e, s_i_d_ref, s_i_q_ref, i_u, i_v, i_w, v_bus, NULL, NULL)
                    == 0;
        }
        /* 过流跳闸（电流环或 vtest 内置）→ M1：只判断不动作 */
        if (APP_PROTECT_ACTION_EN && app_foc_current_is_tripped()) {
            app_foc_isr_emergency();
        }
    }

isr_exit:
    s_execution.sample_sequence++;
    s_execution.timestamp_cycles = t_isr;
    s_execution.epoch = s_current_epoch;
    s_execution.normal_owner = (s_state == APP_FOC_STATE_READY || s_state == APP_FOC_STATE_RUN)
                            && !s_stop_pending && !app_foc_current_vtest_active()
                            && !s_isr_inhibited;
    s_execution.request_expired = request_expired;
    s_execution.request_sequence =
        s_current_source == APP_FOC_CURRENT_SOURCE_MOTION ? s_current_request.sequence : 0;
    s_execution.requested_id_a =
        s_current_source == APP_FOC_CURRENT_SOURCE_MOTION ? s_current_request.id_ref_a : s_i_d_ref;
    s_execution.requested_iq_a = s_current_source == APP_FOC_CURRENT_SOURCE_MOTION
                                   ? s_current_request.iq_ref_a
                                   : s_i_q_ref_cmd;
    s_execution.valid =
        executed && !fault && !degraded && s_execution.normal_owner && g_foc_current_snapshot.valid;
    s_execution.accepted_id_a = s_execution.valid ? g_foc_current_snapshot.i_d_ref_a : 0;
    s_execution.accepted_iq_a = s_execution.valid ? g_foc_current_snapshot.i_q_ref_a : 0;
    s_execution.measured_id_a = g_foc_current_snapshot.i_d_a;
    s_execution.measured_iq_a = g_foc_current_snapshot.i_q_a;
    s_execution.current_limited = s_execution.valid
                               && (s_execution.accepted_id_a != s_execution.requested_id_a
                                   || s_execution.accepted_iq_a != s_execution.requested_iq_a);
    s_execution.voltage_saturated = s_execution.valid && g_foc_current_snapshot.saturated;
    if (!s_execution.valid)
        s_execution.invalid_count++;
    if (s_execution.current_limited)
        s_execution.limited_count++;
    if (s_execution.voltage_saturated)
        s_execution.saturated_count++;
    /* 单一出口：统一计时 + 预算检查（含 OFF / 故障 / 抑制路径；含编码器取帧） */
    {
        uint32_t cycles = intf_clock_get_cycle() - t_isr;

        g_foc_isr_cycles = cycles;
        if (cycles > g_foc_isr_cycles_max) {
            g_foc_isr_cycles_max = cycles;
        }
        if (cycles > s_isr_budget_cycles) {
            g_foc_isr_overruns++;
            /* 孤立尖峰（冷 cache/总线尾延迟）不瞬杀：连续 32 拍超限才判快路径
             * 失控（真触发风暴 = 每拍超限，streak 快速打满）。 */
            s_overrun_streak++;
            if (APP_PROTECT_ACTION_EN && (s_overrun_streak >= APP_FOC_OVR_TRIP_STREAK)) {
                s_isr_disabled = true;
                app_foc_isr_emergency();
            }
        } else {
            s_overrun_streak = 0U;
        }
    }
}

void app_foc_run_once(void) {
    uint32_t t0 = intf_clock_get_cycle();

    s_dt_s = app_foc_measure_dt();
    g_foc_loop_dt_us = (uint32_t)(s_dt_s * 1000000.0f);
    if (s_stop_pending) {
        if (s_state != APP_FOC_STATE_READY && s_state != APP_FOC_STATE_RUN) {
            s_stop_pending = false;
        } else if (++s_stop_ticks >= 120) {
            app_foc_disable();
        } else {
            float scale = s_stop_ticks < 100 ? 1.0f - (float)s_stop_ticks / 100.0f : 0;
            uint32_t st = intf_sys_irq_save();
            s_i_d_ref = s_stop_initial_id * scale;
            s_i_q_ref_cmd = s_stop_initial_iq * scale;
            s_i_q_ref = s_i_q_ref_cmd;
            intf_sys_irq_restore(st);
        }
    }
    app_foc_run_body();
    g_foc_loop_cycles = intf_clock_get_cycle() - t0;
}

int app_foc_enable(void) {
    if (s_stop_pending)
        return -1;
    const app_motor_params_t* motor = app_motor_params_current();
    const app_software_params_t* software = app_software_params_current();
    uint16_t raw;
    bool valid;

    if (!s_initialized) {
        return -1;
    }
    if (!s_angle_ready || !app_foc_current_is_ready()) {
        return -1; /* 角度链/电流环初始化失败：禁止使能 */
    }
    if (s_state == APP_FOC_STATE_FAULT) {
        return -1; /* 需先 app_foc_disable() 回 OFF */
    }
    if (s_state != APP_FOC_STATE_OFF) {
        return 0;  /* 已使能：幂等 */
    }
    if (APP_PROTECT_ACTION_EN && (app_fault_get_state() != APP_FAULT_STATE_NORMAL)) {
        return -2; /* 故障模块非 NORMAL（M1：动作全关时不拦截） */
    }
    if (!app_adc_is_valid()) {
        return -3; /* ADC 采样链无效 */
    }
    if ((app_encoder_get_rotor_raw(&raw, &valid, NULL) != 0) || !valid) {
        return -4; /* 编码器无效 */
    }
    if ((motor->pole_pairs == 0U) || (software->control.limits.duty_max <= 0.5f)
        || (software->control.limits.duty_max > 1.0f)) {
        return -5; /* 参数非法 */
    }
    /* direction 元数据范围 [-1,1] 无法表达"仅 ±1"：运行期显式校验 */
    if ((motor->encoder.direction != 1.0f) && (motor->encoder.direction != -1.0f)) {
        return -1;
    }
    if (!foc_finite(motor->encoder.electrical_offset_rad)) {
        return -1;
    }

    /* 角度链参数重载：pole_pairs / direction / offset 改动在本次 foc on 生效
     * （pole_pairs 为 init 期消费，此处显式重建，避免必须重编译） */
    s_angle_ready = (app_foc_angle_init() == 0);
    if (!s_angle_ready) {
        return -1;
    }

    /* 注：app_3phase_inverter_enable() 内含 +12V 栅极供电稳定等待（约 10ms 阻塞）。
     * 该窗口内桥处于关闭态（PWM 未启动），无电流风险；但 25kHz 环与 L2/L3 保护暂停。
     * 与既有 V/F 启动路径同构；非阻塞桥使能为 v2 项（spec §10）。 */
    /* 清除调试 inv 可能遗留的逐相强制关断（否则该相输出被屏蔽） */
    for (uint8_t i = 0U; i < (uint8_t)APP_3PHASE_COUNT; i++) {
        (void)app_3phase_inverter_release((app_3phase_id_t)i);
    }
    if (app_3phase_inverter_enable() != 0) {
        return -1;
    }
    /* 10ms 阻塞（栅极供电稳定）已在临界区之外完成；此处仅短临界区原子发布状态 */
    app_foc_current_reset();
    app_foc_current_zero_vector();
    s_angle.reset(&s_angle);
    {
        uint32_t st = intf_sys_irq_save();

        if (s_estop_request != NULL) {
            *s_estop_request = 0U;  /* 清除外部锁存，允许本次使能 */
        }
        s_rotor_seq_primed = false; /* 重新建立采样序号基准 */
        s_enc_ts_valid = false;     /* 首拍不注入 dt 尖峰 */
        s_theta_held_valid = false;
        s_enc_degraded_streak = 0U;
        s_overrun_streak = 0U;
        s_isr_dt_valid = false;
        s_emergency_active = false;
        s_isr_inhibited = false;
        s_isr_disabled = false;
        s_speed_limited = false;
        s_dt_valid = false;
        g_foc_fault_request = 0U;
        s_angle_src = APP_FOC_ANGLE_ENCODER;
        s_i_d_ref = 0.0f;
        s_i_q_ref_cmd = 0.0f;
        s_i_q_ref = 0.0f;
        s_state = APP_FOC_STATE_READY;
        app_foc_revoke_current();
        intf_sys_irq_restore(st);
    }
    return 0;
}

void app_foc_disable(void) {
    s_stop_pending = false;
    uint32_t st;

    /* 1) 临界区内先把状态置 OFF 并清零给定：ISR 下一拍立即停止输出。
     *    顺序 = "先 OFF 再复位"，避免 ISR 在复位中途读到半状态。 */
    st = intf_sys_irq_save();
    if (s_estop_request != NULL) {
        *s_estop_request = 0U; /* 清除外部锁存 */
    }
    s_i_d_ref = 0.0f;
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    s_state = APP_FOC_STATE_OFF;
    app_foc_revoke_current();
    s_isr_inhibited = false;
    s_emergency_active = false;
    s_isr_disabled = false;
    s_speed_limited = false;
    s_dt_valid = false;
    s_isr_dt_valid = false;
    s_rotor_seq_primed = false;
    s_enc_ts_valid = false;
    s_theta_held_valid = false;
    s_enc_degraded_streak = 0U;
    s_overrun_streak = 0U;
    g_foc_fault_request = 0U;
    s_angle_src = APP_FOC_ANGLE_ENCODER;
    intf_sys_irq_restore(st);

    /* 2) ISR 已停：安全矢量 + 停桥 + 复位算法（较慢操作放在临界区外）。
     *    正常关闭用 disable()（不强制低），以兼容后续 V/F 调试；
     *    故障路径才用 emergency_stop()（强制低）。 */
    app_foc_current_zero_vector();
    app_3phase_inverter_disable();
    app_foc_current_reset();

    /* 3) 进行中的电角度自校准：不在此反向依赖校准模块；其每拍守卫检测到
     *    状态 ≠ CALIB 即以 STATE 失败退出（≤1ms）。 */
}

int app_foc_set_iq_ref(float i_q_a) {
    float limit;
    uint32_t st;

    if (s_stop_pending || s_current_source != APP_FOC_CURRENT_SOURCE_DIRECT || !foc_finite(i_q_a)) {
        return -1;
    }
    limit = app_software_params_current()->control.limits.i_q_max_a;
    if (!foc_finite(limit) || (limit <= 0.0f)) {
        return -1; /* 限幅值非法：拒绝（防 NaN 穿透/负值反号） */
    }
    if (i_q_a > limit) {
        i_q_a = limit;
    } else if (i_q_a < -limit) {
        i_q_a = -limit;
    }
    st = intf_sys_irq_save();
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_i_q_ref_cmd = i_q_a;
    intf_sys_irq_restore(st);
    return 0;
}

FOC_ATTR_RAMFUNC
bool app_foc_stop_pending(void) { return s_stop_pending; }
int app_foc_set_current_source(app_foc_current_source_t source) {
    uint32_t st = intf_sys_irq_save();
    if ((source != APP_FOC_CURRENT_SOURCE_DIRECT && source != APP_FOC_CURRENT_SOURCE_MOTION)
        || s_stop_pending || app_foc_current_vtest_active()
        || (source == APP_FOC_CURRENT_SOURCE_MOTION && s_angle_src != APP_FOC_ANGLE_ENCODER)
        || (s_current_request.valid
            && (s_current_request.id_ref_a != 0 || s_current_request.iq_ref_a != 0))
        || (s_state != APP_FOC_STATE_OFF
            && (s_state != APP_FOC_STATE_READY || s_i_d_ref != 0 || s_i_q_ref_cmd != 0
                || s_i_q_ref != 0))) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_current_source = source;
    app_foc_revoke_current();
    intf_sys_irq_restore(st);
    return 0;
}
FOC_ATTR_RAMFUNC
app_foc_current_source_t app_foc_get_current_source(void) { return s_current_source; }
FOC_ATTR_RAMFUNC
uint32_t app_foc_current_epoch(void) {
    uint32_t st = intf_sys_irq_save();
    uint32_t epoch = s_current_epoch;
    intf_sys_irq_restore(st);
    return epoch;
}
FOC_ATTR_RAMFUNC
int app_foc_submit_current(const app_foc_current_request_t* request) {
    if (request == NULL || !request->valid || !foc_finite(request->id_ref_a)
        || !foc_finite(request->iq_ref_a))
        return -1;
    uint32_t st = intf_sys_irq_save();
    if (s_current_source != APP_FOC_CURRENT_SOURCE_MOTION || s_stop_pending || s_isr_inhibited
        || app_foc_current_vtest_active()
        || (s_state != APP_FOC_STATE_READY && s_state != APP_FOC_STATE_RUN)
        || request->epoch != s_current_epoch) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_current_request = *request;
    s_current_request_cycle = intf_clock_get_cycle();
    s_current_request_expired = false;
    if (s_state == APP_FOC_STATE_READY && (request->id_ref_a != 0 || request->iq_ref_a != 0))
        s_state = APP_FOC_STATE_RUN;
    intf_sys_irq_restore(st);
    return 0;
}
FOC_ATTR_RAMFUNC
void app_foc_get_execution(app_foc_execution_t* out) {
    if (out != NULL) {
        uint32_t st = intf_sys_irq_save();
        *out = s_execution;
        intf_sys_irq_restore(st);
    }
}
int app_foc_set_duty_ceiling(float ceiling) {
    uint32_t st = intf_sys_irq_save();
    int rc = s_state == APP_FOC_STATE_OFF ? app_foc_current_set_duty_ceiling(ceiling) : -1;
    intf_sys_irq_restore(st);
    return rc;
}
int app_foc_request_stop(void) {
    uint32_t st = intf_sys_irq_save();
    if (s_state == APP_FOC_STATE_FAULT) {
        intf_sys_irq_restore(st);
        app_foc_disable(); /* 保留原foc off清故障锁存路径；故障态无正常缓降 */
        return 0;
    }
    if (s_stop_pending || s_state == APP_FOC_STATE_OFF) {
        intf_sys_irq_restore(st);
        return 0;
    }
    if (s_state != APP_FOC_STATE_READY && s_state != APP_FOC_STATE_RUN) {
        intf_sys_irq_restore(st);
        return -1;
    }
    if (app_foc_current_vtest_active()) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_stop_initial_id = s_i_d_ref;
    s_stop_initial_iq = s_i_q_ref;
    s_stop_ticks = 0;
    s_stop_pending = true;
    app_foc_revoke_current();
    intf_sys_irq_restore(st);
    return 0;
}

int app_foc_set_id_ref(float i_d_a) {
    uint32_t st;

    if (s_stop_pending || s_current_source != APP_FOC_CURRENT_SOURCE_DIRECT || !foc_finite(i_d_a)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_i_d_ref = i_d_a;
    intf_sys_irq_restore(st);
    return 0;
}

int app_foc_set_angle_source(app_foc_angle_source_t src, float theta_e_rad) {
    uint32_t st;

    if ((src != APP_FOC_ANGLE_ENCODER) && (src != APP_FOC_ANGLE_FORCED)) {
        return -1;
    }
    if (!foc_finite(theta_e_rad)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if (s_current_source == APP_FOC_CURRENT_SOURCE_MOTION && s_state != APP_FOC_STATE_CALIB) {
        intf_sys_irq_restore(st);
        return -1;
    }
    if ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN)
        && (s_state != APP_FOC_STATE_CALIB)) {
        intf_sys_irq_restore(st);
        return -1;
    }
    s_angle_src = src;
    s_forced_theta = theta_e_rad;
    intf_sys_irq_restore(st);
    return 0;
}

float app_foc_get_iq_ref(void) { return s_i_q_ref_cmd; }

FOC_ATTR_RAMFUNC
app_foc_state_t app_foc_get_state(void) { return s_state; }

bool app_foc_is_active(void) { return (s_state != APP_FOC_STATE_OFF); }

void app_foc_get_snapshot(app_foc_current_snapshot_t* out) { app_foc_current_get_snapshot(out); }

/**
 * @brief 角度链历史复位（零点/角源切换后：下一拍重新建立序号/时间戳/保持角基准）
 * @note 调用方须持有临界区
 */
static void app_foc_angle_history_reset(void) {
    s_rotor_seq_primed = false;
    s_enc_ts_valid = false;
    s_theta_held_valid = false;
    s_enc_degraded_streak = 0U;
}

/**
 * @brief 方向值是否合法（仅 ±1）
 * @param direction 方向
 * @return true = 合法
 */
static bool app_foc_direction_valid(float direction) {
    return (direction == 1.0f) || (direction == -1.0f);
}

int app_foc_calib_begin(void) {
    uint32_t st = intf_sys_irq_save();

    if (s_stop_pending || ((s_state != APP_FOC_STATE_READY) && (s_state != APP_FOC_STATE_RUN))
        || app_foc_current_vtest_active()) { /* vtest 独占输出中：同一临界区内判定 */
        intf_sys_irq_restore(st);
        return -1;
    }
    app_foc_current_reset();                 /* 临界区内：与 ISR 电流环互斥（仅清零积分器） */
    s_angle_src = APP_FOC_ANGLE_FORCED;
    s_forced_theta = 0.0f;
    s_i_d_ref = 0.0f;
    s_i_q_ref_cmd = 0.0f;
    s_i_q_ref = 0.0f;
    s_state = APP_FOC_STATE_CALIB;
    app_foc_revoke_current();
    intf_sys_irq_restore(st);
    return 0;
}

int app_foc_calib_capture(float voltage, bool voltage_mode) {
    uint32_t st = intf_sys_irq_save();
    int rc =
        s_state == APP_FOC_STATE_CALIB ? app_foc_current_identify_arm(voltage, voltage_mode) : -1;
    intf_sys_irq_restore(st);
    return rc;
}

int app_foc_calib_trial_pi(float kp, float ki) {
    uint32_t st = intf_sys_irq_save();
    int rc = s_state == APP_FOC_STATE_CALIB ? app_foc_current_identify_pi(kp, ki) : -1;
    intf_sys_irq_restore(st);
    return rc;
}
int app_foc_calib_voltage(float voltage, uint32_t cycles) {
    uint32_t st = intf_sys_irq_save();
    int rc =
        s_state == APP_FOC_STATE_CALIB ? app_foc_current_identify_voltage(voltage, cycles) : -1;
    intf_sys_irq_restore(st);
    return rc;
}
int app_foc_calib_pulse(float amplitude, uint16_t half_period) {
    uint32_t st = intf_sys_irq_save();
    int rc = s_state == APP_FOC_STATE_CALIB ? app_foc_current_identify_pulse(amplitude, half_period)
                                            : -1;
    intf_sys_irq_restore(st);
    return rc;
}

void app_foc_calib_end(void) {
    uint32_t st = intf_sys_irq_save();

    if (s_state == APP_FOC_STATE_CALIB) {
        s_i_d_ref = 0.0f;
        s_i_q_ref_cmd = 0.0f;
        s_i_q_ref = 0.0f;
        /* 试用零点作废：恢复运行零点（set_offset 同时清除 ωe 差分历史） */
        s_angle.set_offset(&s_angle, s_angle_offset_rad, s_angle_direction);
        s_angle_src = APP_FOC_ANGLE_ENCODER;
        app_foc_angle_history_reset();
        app_foc_current_reset();
        s_state = APP_FOC_STATE_READY;
        app_foc_revoke_current();
    }
    intf_sys_irq_restore(st);
}

int app_foc_calib_set_excitation(float theta_e_rad, float i_d_ref, float i_q_ref) {
    uint32_t st;
    int rc = -1;

    if (!foc_finite(theta_e_rad) || !foc_finite(i_d_ref) || !foc_finite(i_q_ref)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if (s_state == APP_FOC_STATE_CALIB) {
        s_angle_src = APP_FOC_ANGLE_FORCED;
        s_forced_theta = theta_e_rad;
        s_i_d_ref = i_d_ref;
        s_i_q_ref = i_q_ref;
        rc = 0;
    }
    intf_sys_irq_restore(st);
    return rc;
}

int app_foc_calib_use_trial_offset(float offset_rad, float direction) {
    uint32_t st;
    int rc = -1;

    if (!foc_finite(offset_rad) || !app_foc_direction_valid(direction)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if (s_state == APP_FOC_STATE_CALIB) {
        s_angle.set_offset(&s_angle, offset_rad, direction); /* 运行镜像不变（end 恢复） */
        s_angle_src = APP_FOC_ANGLE_ENCODER;
        app_foc_angle_history_reset();
        app_foc_current_reset(); /* 角源切换：清 PI 积分，避免强制角阶段积分量带入 */
        rc = 0;
    }
    intf_sys_irq_restore(st);
    return rc;
}

int app_foc_calib_set_current(float i_d_ref, float i_q_ref) {
    uint32_t st;
    int rc = -1;

    if (!foc_finite(i_d_ref) || !foc_finite(i_q_ref)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if (s_state == APP_FOC_STATE_CALIB) {
        s_i_d_ref = i_d_ref;
        s_i_q_ref = i_q_ref;
        rc = 0;
    }
    intf_sys_irq_restore(st);
    return rc;
}

int app_foc_apply_encoder_offset(float offset_rad, float direction) {
    uint32_t st;
    int rc = -1;

    if (!foc_finite(offset_rad) || !app_foc_direction_valid(direction)) {
        return -1;
    }
    st = intf_sys_irq_save();
    if (s_state != APP_FOC_STATE_CALIB) {
        s_angle.set_offset(&s_angle, offset_rad, direction);
        s_angle_offset_rad = offset_rad;
        s_angle_direction = direction;
        app_foc_angle_history_reset();
        rc = 0;
    }
    intf_sys_irq_restore(st);
    return rc;
}

void app_foc_get_encoder_offset(float* offset_rad, float* direction) {
    if (offset_rad != NULL) {
        *offset_rad = s_angle_offset_rad;
    }
    if (direction != NULL) {
        *direction = s_angle_direction;
    }
}

bool app_foc_fault_gate(void) { return (s_state == APP_FOC_STATE_FAULT); }

bool app_foc_isr_inhibited(void) { return s_isr_inhibited; }
