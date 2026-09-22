/**
 * @file    drv_hrpwm.c
 * @brief   HRPWM 驱动 - HPM PWM 硬件实现（移相/死区/故障保护/触发比较）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_hrpwm.h"

#include "board.h"
#include "hpm_clock_drv.h"
#include "hpm_common.h"
#include "hpm_interrupt.h"
#include "hpm_pwm_drv.h"
#include "hpm_soc.h"
#include "hpm_soc_irq.h"
#include "hpm_trgm_drv.h"
#include "hpm_trgmmux_src.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>

#ifndef ATTR_RAMFUNC
#define ATTR_RAMFUNC __attribute__((section(".fast")))
#endif

/* HRPWM pins configured in pinmux.c for current board:
 * PA25 -> PWM0_P_1 (reserved / not used by App HRPWM platform)
 * PA28 -> PWM1_P_4, PA29 -> PWM1_P_5
 * PA30 -> PWM1_P_6, PA31 -> PWM1_P_7
 */
#define BOARD_APP_HRPWM0                 HPM_PWM0
#define BOARD_APP_HRPWM1                 HPM_PWM1
#define BOARD_APP_HRPWM_CLOCK_NAME       clock_mot0
#define BOARD_APP_HRPWM_PWM0_PAIR0_OUT   (0U)
#define BOARD_APP_HRPWM_PWM0_PAIR1_OUT   (2U)
#define BOARD_APP_HRPWM_PWM1_PAIR0_OUT   (4U)
#define BOARD_APP_HRPWM_PWM1_PAIR1_OUT   (6U)

#define HRPWM_INSTANCE_COUNT   (2U)
#define HRPWM_OUTPUTS_PER_INST (PWM_SOC_PWM_MAX_COUNT)
#define HRPWM_CHANNEL_COUNT    (8U)

#define HRPWM_CMP_START_INDEX(pwm_index)  ((uint8_t)((pwm_index) * 2U))
#define HRPWM_PHASE_RUNTIME_GUARD_MAX_DEG (89.0f)

/* 配置：是否使用28位扩展计数器
 * 0 = 使用24位计数器（默认，兼容性好）
 * 1 = 使用28位计数器（24位主计数器 + 4位扩展计数器，更高分辨率） */
#ifndef HRPWM_USE_EXTENDED_COUNTER
# define HRPWM_USE_EXTENDED_COUNTER (1U)
#endif

/* 计数器最大值 */
#define HRPWM_RELOAD_MAX_24BIT (PWM_RLD_RLD_GET(PWM_RLD_RLD_MASK))
#define HRPWM_RELOAD_MAX_28BIT (0x0FFFFFFFUL)

#if HRPWM_USE_EXTENDED_COUNTER
# define HRPWM_RELOAD_MAX_VALUE HRPWM_RELOAD_MAX_28BIT
#else
# define HRPWM_RELOAD_MAX_VALUE HRPWM_RELOAD_MAX_24BIT
#endif

static_assert(
    (HRPWM_CMP_START_INDEX(PWM_SOC_PWM_MAX_COUNT - 1U) + 1U) < PWM_SOC_CMP_MAX_COUNT,
    "HRPWM compare mapping exceeds PWM compare resource count");

/**
 * @brief HRPWM 逻辑通道到物理通道的映射
 */
typedef struct {
    intf_hrpwm_ch_t channel; /**< 逻辑通道号 */
    uint8_t instance;        /**< 实例号 */
    uint8_t pair;            /**< 实例内配对序号 */
    uint8_t pwm_index;       /**< 物理通道起始索引（偶数） */
    uint8_t cmp_start_index; /**< CMP 起始索引 */
} hrpwm_channel_map_t;

/**
 * @brief HRPWM 通道状态
 */
typedef struct {
    bool configured;          /**< 是否已配置 */
    bool started;             /**< 是否已启动输出 */
    float duty;               /**< 占空比 [0.0-1.0] */
    uint32_t reload;          /**< 重载值 */
    uint8_t ex_reload;        /**< 扩展重载值 */
    uint8_t jitter_cmp;       /**< 抖动比较值 */
    intf_hrpwm_align_t align; /**< 对齐方式 */
    bool invert_high_side;    /**< 高边反相 */
    bool invert_low_side;     /**< 低边反相 */
} hrpwm_channel_state_t;

/**
 * @brief HRPWM 移相状态
 */
typedef struct {
    bool active;          /**< 是否启用移相 */
    uint8_t ref_pair;     /**< 参考配对 */
    uint8_t target_pair;  /**< 目标配对 */
    float phase_deg;      /**< 移相角 [deg] */
    uint32_t phase_count; /**< 移相计数值 */
} hrpwm_phase_state_t;

/**
 * @brief HRPWM 移相限值
 */
typedef struct {
    float max_phase_deg;   /**< 最大移相角 [deg] */
    float max_duty_ref;    /**< 参考配对最大占空比 */
    float max_duty_target; /**< 目标配对最大占空比 */
} hrpwm_phase_limit_t;

/**
 * @brief HRPWM 实例状态
 */
typedef struct {
    PWM_Type* base;                                         /**< PWM 寄存器基地址 */
    clock_name_t clock_name;                                /**< 外设时钟 */
    uint32_t frequency_hz;                                  /**< 频率 [Hz] */
    uint32_t reload;                                        /**< 重载值 */
    uint8_t ex_reload;                                      /**< 扩展重载值 */
    bool fault_configured;                                  /**< 是否已配置故障保护 */
    uint8_t force_mask;                                     /**< 强制输出通道掩码 */
    hrpwm_channel_state_t channels[HRPWM_OUTPUTS_PER_INST]; /**< 各通道状态 */
    hrpwm_phase_state_t phase;                              /**< 移相状态 */
    hrpwm_phase_limit_t phase_limit;                        /**< 移相限值 */
} hrpwm_instance_state_t;

ATTR_PLACE_AT_FAST_RAM_BSS static hrpwm_instance_state_t s_hrpwm_instances[HRPWM_INSTANCE_COUNT];

/**
 * @brief HRPWM 比较值对
 */
typedef struct {
    uint32_t cmp_begin; /**< 起始比较值 */
    uint32_t cmp_end;   /**< 结束比较值 */
} hrpwm_cmp_pair_t;

ATTR_PLACE_AT_FAST_RAM_INIT static const hrpwm_channel_map_t s_hrpwm_channel_maps[] = {
    {
     .channel = BOARD_APP_HRPWM_PWM0_PAIR0_OUT,
     .instance = 0,
     .pair = 0,
     .pwm_index = BOARD_APP_HRPWM_PWM0_PAIR0_OUT,
     .cmp_start_index = HRPWM_CMP_START_INDEX(BOARD_APP_HRPWM_PWM0_PAIR0_OUT),
     },
    {
     .channel = BOARD_APP_HRPWM_PWM0_PAIR1_OUT,
     .instance = 0,
     .pair = 1,
     .pwm_index = BOARD_APP_HRPWM_PWM0_PAIR1_OUT,
     .cmp_start_index = HRPWM_CMP_START_INDEX(BOARD_APP_HRPWM_PWM0_PAIR1_OUT),
     },
    {
     .channel = BOARD_APP_HRPWM_PWM1_PAIR0_OUT,
     .instance = 1,
     .pair = 0,
     .pwm_index = BOARD_APP_HRPWM_PWM1_PAIR0_OUT,
     .cmp_start_index = HRPWM_CMP_START_INDEX(BOARD_APP_HRPWM_PWM1_PAIR0_OUT),
     },
    {
     .channel = BOARD_APP_HRPWM_PWM1_PAIR1_OUT,
     .instance = 1,
     .pair = 1,
     .pwm_index = BOARD_APP_HRPWM_PWM1_PAIR1_OUT,
     .cmp_start_index = HRPWM_CMP_START_INDEX(BOARD_APP_HRPWM_PWM1_PAIR1_OUT),
     },
    {
     /* PWM1 ch0/1（PA24/25 → HIN3/LIN3，W 相）：虚拟通道 8（物理索引 0） */
     .channel = 8,
     .instance = 1,
     .pair = 2,
     .pwm_index = 0,
     .cmp_start_index = HRPWM_CMP_START_INDEX(0),
     },
};

/**
 * @brief 取实例完整重载值（24bit + 4bit 扩展）
 * @param inst 实例号
 * @return 完整重载值
 */
ATTR_RAMFUNC
static inline uint32_t hrpwm_get_full_reload(uint8_t inst) {
#if HRPWM_USE_EXTENDED_COUNTER
    return ((uint32_t)s_hrpwm_instances[inst].ex_reload << 24U) | s_hrpwm_instances[inst].reload;
#else
    return s_hrpwm_instances[inst].reload;
#endif
}

static int hrpwm_apply_duty(const hrpwm_channel_map_t* map);

/**
 * @brief 初始化实例静态信息（基地址/时钟/移相限值，幂等）
 */
static void hrpwm_init_instances(void) {
    s_hrpwm_instances[0].base = BOARD_APP_HRPWM0;
    s_hrpwm_instances[0].clock_name = BOARD_APP_HRPWM_CLOCK_NAME;
    s_hrpwm_instances[0].ex_reload = 0U;
    s_hrpwm_instances[0].phase_limit.max_phase_deg = 180.0f;
    s_hrpwm_instances[0].phase_limit.max_duty_ref = 1.0f;
    s_hrpwm_instances[0].phase_limit.max_duty_target = 1.0f;

    s_hrpwm_instances[1].base = BOARD_APP_HRPWM1;
    s_hrpwm_instances[1].clock_name = BOARD_APP_HRPWM_CLOCK_NAME;
    s_hrpwm_instances[1].ex_reload = 0U;
    s_hrpwm_instances[1].phase_limit.max_phase_deg = 180.0f;
    s_hrpwm_instances[1].phase_limit.max_duty_ref = 1.0f;
    s_hrpwm_instances[1].phase_limit.max_duty_target = 1.0f;
}

/**
 * @brief 获取实例 PWM 基地址
 * @param inst 实例号
 * @return PWM 基地址；越界返回 NULL
 */
ATTR_RAMFUNC
static inline PWM_Type* hrpwm_get_base(uint8_t inst) {
    return (inst < HRPWM_INSTANCE_COUNT) ? s_hrpwm_instances[inst].base : NULL;
}

/**
 * @brief 按实例与配对序号查找通道映射
 * @param inst 实例号
 * @param pair 配对序号
 * @return 通道映射；未找到返回 NULL
 */
static const hrpwm_channel_map_t* hrpwm_get_pair_map(uint8_t inst, uint8_t pair) {
    for (size_t i = 0; i < sizeof(s_hrpwm_channel_maps) / sizeof(s_hrpwm_channel_maps[0]); i++) {
        if ((s_hrpwm_channel_maps[i].instance == inst) && (s_hrpwm_channel_maps[i].pair == pair)) {
            return &s_hrpwm_channel_maps[i];
        }
    }
    return NULL;
}

/**
 * @brief 按逻辑通道号查找通道映射（同 pair 两通道映射到同一条目）
 * @param ch 逻辑通道号
 * @return 通道映射；未找到返回 NULL
 */
ATTR_RAMFUNC
static const hrpwm_channel_map_t* hrpwm_get_channel_map(intf_hrpwm_ch_t ch) {
    intf_hrpwm_ch_t pair_channel = (intf_hrpwm_ch_t)(ch & (uint8_t)~1U);

    for (size_t i = 0; i < sizeof(s_hrpwm_channel_maps) / sizeof(s_hrpwm_channel_maps[0]); i++) {
        if (s_hrpwm_channel_maps[i].channel == pair_channel) {
            return &s_hrpwm_channel_maps[i];
        }
    }
    return NULL;
}

/**
 * @brief 校验占空比是否合法
 * @param duty 占空比
 * @return true = 合法（非 NaN 且 [0,1]）
 */
ATTR_RAMFUNC
static bool hrpwm_is_valid_duty(float duty) {
    return (duty == duty) && (duty >= 0.0f) && (duty <= 1.0f);
}

/**
 * @brief 校验对齐方式是否合法
 * @param align 对齐方式
 * @return true = 合法
 */
static bool hrpwm_is_valid_align(intf_hrpwm_align_t align) {
    return (align == INTF_HRPWM_ALIGN_EDGE) || (align == INTF_HRPWM_ALIGN_CENTER);
}

/**
 * @brief 占空比转换为比较计数值
 * @param reload 重载值
 * @param duty 占空比
 * @return 比较计数值
 */
ATTR_RAMFUNC
static uint32_t hrpwm_duty_to_cmp_count(uint32_t reload, float duty) {
    return (uint32_t)((float)reload * duty);
}

/**
 * @brief 计算中心对齐比较值对
 * @param reload 重载值
 * @param duty 占空比
 * @return 比较值对（cmp_begin/cmp_end）
 */
ATTR_RAMFUNC
static hrpwm_cmp_pair_t hrpwm_calc_center_aligned_cmp(uint32_t reload, float duty) {
    hrpwm_cmp_pair_t cmp;

    /* 100% duty cycle: output stays high permanently.
     * Set both CMP values beyond reload so Counter never matches.
     * 注意：HPM5300 PWM 为单向递增计数器（0 → reload → 回卷），
     * 所谓“中心对齐”由 cmp_begin/cmp_end 围绕 reload/2 对称摆放合成，
     * 周期 = (reload+1) 个 PWM 时钟。CMP = reload + 1 永不匹配。 */
    if (duty >= 1.0f) {
        cmp.cmp_begin = reload + 1U;
        cmp.cmp_end = reload + 1U;
        return cmp;
    }

    /* 0% duty cycle: output stays low permanently.
     * Set both CMP values to 0 so Counter matches immediately at zero.
     * This forces output low for the entire period. */
    if (duty <= 0.0f) {
        cmp.cmp_begin = 0U;
        cmp.cmp_end = 0U;
        return cmp;
    }

    /* Normal duty cycle: calculate symmetric compare values */
    uint32_t target_cmp = hrpwm_duty_to_cmp_count(reload, duty);
    cmp.cmp_begin = (reload - target_cmp) >> 1;
    cmp.cmp_end = (reload + target_cmp) >> 1;

    /* Ensure CMP values are not zero to avoid edge case at Counter = 0.
     * Using 1 instead of 0 prevents unintended Compare Match at period start. */
    if (cmp.cmp_begin == 0U) {
        cmp.cmp_begin = 1U;
    }
    if (cmp.cmp_end == 0U) {
        cmp.cmp_end = 1U;
    }

    /* Ensure begin <= end (swap if needed) */
    if (cmp.cmp_begin > cmp.cmp_end) {
        uint32_t cmp_swap = cmp.cmp_begin;
        cmp.cmp_begin = cmp.cmp_end;
        cmp.cmp_end = cmp_swap;
    }

    return cmp;
}

/**
 * @brief 计算边沿对齐比较值对
 * @param reload 重载值
 * @param duty 占空比
 * @return 比较值对（cmp_begin/cmp_end）
 */
ATTR_RAMFUNC
static hrpwm_cmp_pair_t hrpwm_calc_edge_aligned_cmp(uint32_t reload, float duty) {
    hrpwm_cmp_pair_t cmp;

    /* 100% duty cycle: output stays high permanently.
     * Set both CMP values beyond reload so Counter never matches. */
    if (duty >= 1.0f) {
        cmp.cmp_begin = reload + 1U;
        cmp.cmp_end = reload + 1U;
        return cmp;
    }

    /* 0% duty cycle: output stays low permanently.
     * Set both CMP values to 0 so Counter matches immediately. */
    if (duty <= 0.0f) {
        cmp.cmp_begin = 0U;
        cmp.cmp_end = 0U;
        return cmp;
    }

    /* Normal duty cycle */
    uint32_t target_cmp = hrpwm_duty_to_cmp_count(reload, duty);
    cmp.cmp_begin = reload - target_cmp;
    cmp.cmp_end = reload;

    /* Ensure CMP value is not zero to avoid edge case at Counter = 0 */
    if (cmp.cmp_begin == 0U) {
        cmp.cmp_begin = 1U;
    }

    return cmp;
}

/**
 * @brief 按对齐方式计算比较值对
 * @param reload 重载值
 * @param duty 占空比
 * @param align 对齐方式
 * @return 比较值对
 */
ATTR_RAMFUNC
static hrpwm_cmp_pair_t hrpwm_calc_cmp_pair(uint32_t reload, float duty, intf_hrpwm_align_t align) {
    if (align == INTF_HRPWM_ALIGN_CENTER) {
        return hrpwm_calc_center_aligned_cmp(reload, duty);
    }

    return hrpwm_calc_edge_aligned_cmp(reload, duty);
}

/**
 * @brief 写入一对 CMP 值（含扩展位），并解锁影子寄存器
 * @param base PWM 基地址
 * @param cmp_start_index CMP 起始索引
 * @param cmp 比较值对
 */
ATTR_RAMFUNC
static void
    hrpwm_write_cmp_pair(PWM_Type* base, uint8_t cmp_start_index, const hrpwm_cmp_pair_t* cmp) {
#if HRPWM_USE_EXTENDED_COUNTER
    uint16_t ex_begin = (uint16_t)((cmp->cmp_begin >> 24U) & 0x0FU);
    uint16_t ex_end = (uint16_t)((cmp->cmp_end >> 24U) & 0x0FU);
    uint32_t begin_24bit = cmp->cmp_begin & HRPWM_RELOAD_MAX_24BIT;
    uint32_t end_24bit = cmp->cmp_end & HRPWM_RELOAD_MAX_24BIT;

    pwm_shadow_register_unlock(base);
    pwm_cmp_update_cmp_value(base, cmp_start_index, begin_24bit, ex_begin);
    pwm_cmp_update_cmp_value(base, cmp_start_index + 1U, end_24bit, ex_end);
#else
    pwm_shadow_register_unlock(base);
    pwm_cmp_update_cmp_value(base, cmp_start_index, cmp->cmp_begin, 0);
    pwm_cmp_update_cmp_value(base, cmp_start_index + 1U, cmp->cmp_end, 0);
#endif
}

/**
 * @brief 设置配对高/低边输出反相（含移相窗口反相）
 * @param base PWM 基地址
 * @param map 通道映射
 * @param invert_window 是否反相移相窗口
 */
ATTR_RAMFUNC
static void hrpwm_set_pair_output_invert(
    PWM_Type* base, const hrpwm_channel_map_t* map, bool invert_window) {
    pwm_output_channel_t ch_cfg;
    const hrpwm_channel_state_t* channel;

    if (base == NULL || map == NULL) {
        return;
    }

    channel = &s_hrpwm_instances[map->instance].channels[map->pwm_index];

    ch_cfg.cmp_start_index = map->cmp_start_index;
    ch_cfg.cmp_end_index = map->cmp_start_index + 1U;

    ch_cfg.invert_output = channel->invert_high_side ^ invert_window;
    pwm_config_output_channel(base, map->pwm_index, &ch_cfg);

    ch_cfg.invert_output = channel->invert_low_side ^ invert_window;
    pwm_config_output_channel(base, map->pwm_index + 1U, &ch_cfg);
}

/**
 * @brief 判断给定物理通道是否为移相目标
 * @param inst 实例号
 * @param pwm_index 物理通道索引
 * @return true = 是当前移相目标
 */
static bool hrpwm_phase_targets_channel(uint8_t inst, uint8_t pwm_index) {
    hrpwm_phase_state_t* phase;

    if (inst >= HRPWM_INSTANCE_COUNT) {
        return false;
    }

    phase = &s_hrpwm_instances[inst].phase;
    if (!phase->active) {
        return false;
    }

    if (phase->target_pair == 0U) {
        return pwm_index == (uint8_t)(inst * 4U);
    }

    return pwm_index == (uint8_t)(inst * 4U + 2U);
}

/**
 * @brief 判断实例是否有配对已启动（移相运行时判定）
 * @param inst 实例号
 * @return true = 有配对在运行
 */
static bool hrpwm_phase_is_running(uint8_t inst) {
    const hrpwm_channel_map_t* ref_map;
    const hrpwm_channel_map_t* target_map;
    hrpwm_phase_state_t* phase;

    if (inst >= HRPWM_INSTANCE_COUNT) {
        return false;
    }

    phase = &s_hrpwm_instances[inst].phase;
    if (!phase->active) {
        return false;
    }

    ref_map = hrpwm_get_pair_map(inst, phase->ref_pair);
    target_map = hrpwm_get_pair_map(inst, phase->target_pair);
    if (ref_map == NULL || target_map == NULL) {
        return false;
    }

    return s_hrpwm_instances[inst].channels[ref_map->pwm_index].started
        || s_hrpwm_instances[inst].channels[target_map->pwm_index].started;
}

/**
 * @brief 判断给定物理通道是否为移相参考
 * @param inst 实例号
 * @param pwm_index 物理通道索引
 * @return true = 是当前移相参考
 */
static bool hrpwm_phase_refs_channel(uint8_t inst, uint8_t pwm_index) {
    hrpwm_phase_state_t* phase;

    if (inst >= HRPWM_INSTANCE_COUNT) {
        return false;
    }

    phase = &s_hrpwm_instances[inst].phase;
    if (!phase->active) {
        return false;
    }

    if (phase->ref_pair == 0U) {
        return pwm_index == (uint8_t)(inst * 4U);
    }

    return pwm_index == (uint8_t)(inst * 4U + 2U);
}

/**
 * @brief 恢复指定配对的常规波形（取消反相并重算占空比）
 * @param inst 实例号
 * @param pair 配对序号
 * @return 0 = 成功；-1 = 映射或基地址无效
 */
static int hrpwm_restore_pair_waveform(uint8_t inst, uint8_t pair) {
    PWM_Type* base;
    const hrpwm_channel_map_t* map;

    map = hrpwm_get_pair_map(inst, pair);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }

    hrpwm_set_pair_output_invert(base, map, false);
    return hrpwm_apply_duty(map);
}

/**
 * @brief 按当前移相状态更新目标配对波形
 * @param inst 实例号
 * @return 0 = 成功；-1 = 状态/参数无效
 */
static int hrpwm_apply_phase(uint8_t inst) {
    hrpwm_instance_state_t* inst_state;
    const hrpwm_channel_map_t* ref_map;
    const hrpwm_channel_map_t* target_map;
    hrpwm_channel_state_t* ref_ch;
    hrpwm_channel_state_t* target_ch;
    PWM_Type* base;
    uint32_t full_reload;
    uint32_t period_count;
    hrpwm_cmp_pair_t ref_cmp;
    hrpwm_cmp_pair_t target_cmp;
    uint32_t new_begin;
    uint32_t new_end;
    bool invert_window = false;

    if (inst >= HRPWM_INSTANCE_COUNT) {
        return -1;
    }

    inst_state = &s_hrpwm_instances[inst];
    if (!inst_state->phase.active) {
        return 0;
    }

    ref_map = hrpwm_get_pair_map(inst, inst_state->phase.ref_pair);
    target_map = hrpwm_get_pair_map(inst, inst_state->phase.target_pair);
    if (ref_map == NULL || target_map == NULL) {
        return -1;
    }

    ref_ch = &inst_state->channels[ref_map->pwm_index];
    target_ch = &inst_state->channels[target_map->pwm_index];
    if (!ref_ch->configured || !target_ch->configured) {
        return -1;
    }

    if (ref_ch->duty > inst_state->phase_limit.max_duty_ref
        || target_ch->duty > inst_state->phase_limit.max_duty_target) {
        return -1;
    }

    base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }

    full_reload = hrpwm_get_full_reload(inst);
    period_count = full_reload + 1U;
    ref_cmp = hrpwm_calc_cmp_pair(full_reload, ref_ch->duty, ref_ch->align);

    if (ref_cmp.cmp_begin > full_reload || ref_cmp.cmp_end > full_reload) {
        hrpwm_set_pair_output_invert(base, target_map, false);
        hrpwm_write_cmp_pair(base, target_map->cmp_start_index, &ref_cmp);
        return 0;
    }

    new_begin = ref_cmp.cmp_begin + inst_state->phase.phase_count;
    new_end = ref_cmp.cmp_end + inst_state->phase.phase_count;

    target_cmp.cmp_begin = new_begin % period_count;
    target_cmp.cmp_end = new_end % period_count;

    if (target_cmp.cmp_begin > target_cmp.cmp_end) {
        uint32_t temp = target_cmp.cmp_begin;
        target_cmp.cmp_begin = target_cmp.cmp_end;
        target_cmp.cmp_end = temp;
        invert_window = true;
    }

    hrpwm_set_pair_output_invert(base, target_map, invert_window);
    hrpwm_write_cmp_pair(base, target_map->cmp_start_index, &target_cmp);
    return 0;
}

/**
 * @brief 按通道状态重算并写入比较值
 * @param map 通道映射
 * @return 0 = 成功；-1 = 映射或基地址无效
 */
ATTR_RAMFUNC
static int hrpwm_apply_duty(const hrpwm_channel_map_t* map) {
    PWM_Type* base;
    const hrpwm_channel_state_t* channel;
    hrpwm_cmp_pair_t cmp;
    uint32_t full_reload;

    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    channel = &s_hrpwm_instances[map->instance].channels[map->pwm_index];

    full_reload = hrpwm_get_full_reload(map->instance);

    cmp = hrpwm_calc_cmp_pair(full_reload, channel->duty, channel->align);
    hrpwm_set_pair_output_invert(base, map, false);
    hrpwm_write_cmp_pair(base, map->cmp_start_index, &cmp);

    return 0;
}

/**
 * @brief 设置实例频率（重载值）并刷新已配置通道波形
 * @param inst 实例号
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 参数非法或频率超范围
 */
static int hrpwm_apply_frequency(uint8_t inst, uint32_t frequency_hz) {
    uint32_t clock_hz;
    uint32_t reload;
    PWM_Type* base = hrpwm_get_base(inst);
    clock_name_t clock_name;

    if ((inst >= HRPWM_INSTANCE_COUNT) || (base == NULL) || (frequency_hz == 0U)) {
        return -1;
    }

    clock_name = s_hrpwm_instances[inst].clock_name;
    clock_add_to_group(clock_name, 0);
    clock_hz = clock_get_frequency(clock_name);
    if (clock_hz <= frequency_hz) {
        return -1;
    }

    reload = (clock_hz / frequency_hz) - 1U;
    if (reload >= HRPWM_RELOAD_MAX_VALUE) {
        return -1;
    }

    s_hrpwm_instances[inst].frequency_hz = frequency_hz;
    s_hrpwm_instances[inst].reload = reload;
    s_hrpwm_instances[inst].ex_reload = 0U;

#if HRPWM_USE_EXTENDED_COUNTER
    if (reload > HRPWM_RELOAD_MAX_24BIT) {
        s_hrpwm_instances[inst].ex_reload = (uint8_t)((reload >> 24U) & 0x0FU);
        s_hrpwm_instances[inst].reload = reload & HRPWM_RELOAD_MAX_24BIT;
    }
#endif

    pwm_shadow_register_unlock(base);
    pwm_set_reload(base, s_hrpwm_instances[inst].ex_reload, s_hrpwm_instances[inst].reload);
    pwm_set_start_count(base, 0, 0);
    pwm_issue_shadow_register_lock_event(base);

    for (size_t i = 0; i < sizeof(s_hrpwm_channel_maps) / sizeof(s_hrpwm_channel_maps[0]); i++) {
        const hrpwm_channel_map_t* map = &s_hrpwm_channel_maps[i];
        if ((map->instance == inst) && s_hrpwm_instances[inst].channels[map->pwm_index].configured) {
            s_hrpwm_instances[inst].channels[map->pwm_index].reload = s_hrpwm_instances[inst].reload;
            s_hrpwm_instances[inst].channels[map->pwm_index].ex_reload =
                s_hrpwm_instances[inst].ex_reload;
            if (hrpwm_apply_duty(map) != 0) {
                return -1;
            }
        }
    }

    if (hrpwm_apply_phase(inst) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief 死区时间（ns）转换为半周期计数值（四舍五入）
 * @param inst 实例号
 * @param deadtime_ns 死区时间 [ns]
 * @return 半周期计数值
 */
static uint32_t hrpwm_ns_to_deadtime_cycles(uint8_t inst, uint32_t deadtime_ns) {
    uint32_t clock_hz = clock_get_frequency(s_hrpwm_instances[inst].clock_name);
    uint32_t clock_period_ns = 1000000000U / clock_hz;
    return (deadtime_ns + clock_period_ns / 2) / clock_period_ns;
}

/**
 * @brief 初始化一对 PWM 通道（频率/死区/反相/比较值）
 * @param ch 逻辑通道号
 * @param cfg 配对配置
 * @return 0 = 成功；-1 = 参数非法或 SDK 配置失败
 */
static int hrpwm_init_pair(intf_hrpwm_ch_t ch, const intf_hrpwm_pair_cfg_t* cfg) {
    const hrpwm_channel_map_t* map;
    uint8_t inst;
    PWM_Type* base;
    pwm_pair_config_t pair_config = {0};
    pwm_cmp_config_t cmp_config[2] = {0};

    map = hrpwm_get_channel_map(ch);
    if ((cfg == NULL) || (map == NULL) || !hrpwm_is_valid_duty(cfg->duty)
        || !hrpwm_is_valid_align(cfg->align)) {
        return -1;
    }

    inst = map->instance;
    base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }

    if (hrpwm_apply_frequency(inst, cfg->frequency_hz) != 0) {
        return -1;
    }

    pwm_get_default_pwm_pair_config(base, &pair_config);

    pair_config.pwm[0].enable_output = false;
    pair_config.pwm[0].invert_output = cfg->invert_high_side;
    pair_config.pwm[0].dead_zone_in_half_cycle =
        hrpwm_ns_to_deadtime_cycles(inst, cfg->deadtime_ns);

    pair_config.pwm[1].enable_output = false;
    pair_config.pwm[1].invert_output = cfg->invert_low_side;
    pair_config.pwm[1].dead_zone_in_half_cycle =
        hrpwm_ns_to_deadtime_cycles(inst, cfg->deadtime_ns);

    cmp_config[0].mode = pwm_cmp_mode_output_compare;
#if HRPWM_USE_EXTENDED_COUNTER
    cmp_config[0].cmp = s_hrpwm_instances[inst].reload;
    cmp_config[0].enable_ex_cmp = (s_hrpwm_instances[inst].ex_reload > 0U);
    cmp_config[0].ex_cmp = s_hrpwm_instances[inst].ex_reload;
#else
    cmp_config[0].cmp = s_hrpwm_instances[inst].reload + 1;
#endif
    cmp_config[0].jitter_cmp = cfg->jitter_cmp;
    cmp_config[0].update_trigger = pwm_shadow_register_update_on_modify;

    cmp_config[1].mode = pwm_cmp_mode_output_compare;
    cmp_config[1].cmp = s_hrpwm_instances[inst].reload;
#if HRPWM_USE_EXTENDED_COUNTER
    cmp_config[1].enable_ex_cmp = (s_hrpwm_instances[inst].ex_reload > 0U);
    cmp_config[1].ex_cmp = s_hrpwm_instances[inst].ex_reload;
#endif
    cmp_config[1].jitter_cmp = cfg->jitter_cmp;
    cmp_config[1].update_trigger = pwm_shadow_register_update_on_modify;

    if (pwm_setup_waveform_in_pair(
            base, map->pwm_index, &pair_config, map->cmp_start_index, cmp_config, 2)
        != status_success) {
        return -1;
    }

    s_hrpwm_instances[inst].channels[map->pwm_index].configured = true;
    s_hrpwm_instances[inst].channels[map->pwm_index].duty = cfg->duty;
    s_hrpwm_instances[inst].channels[map->pwm_index].reload = s_hrpwm_instances[inst].reload;
    s_hrpwm_instances[inst].channels[map->pwm_index].ex_reload = s_hrpwm_instances[inst].ex_reload;
    s_hrpwm_instances[inst].channels[map->pwm_index].jitter_cmp = cfg->jitter_cmp;
    s_hrpwm_instances[inst].channels[map->pwm_index].align = cfg->align;
    s_hrpwm_instances[inst].channels[map->pwm_index].invert_high_side = cfg->invert_high_side;
    s_hrpwm_instances[inst].channels[map->pwm_index].invert_low_side = cfg->invert_low_side;

    return hrpwm_apply_duty(map);
}

/**
 * @brief 设置占空比（拒绝移相目标通道，联动更新移相参考）
 * @param ch 逻辑通道号
 * @param duty 占空比
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
ATTR_RAMFUNC
static int hrpwm_set_duty(intf_hrpwm_ch_t ch, float duty) {
    const hrpwm_channel_map_t* map;
    hrpwm_instance_state_t* inst_state;

    map = hrpwm_get_channel_map(ch);
    /* instance 越界防御：Release(-O2) 下编译器无法证明映射表取值 < 实例数，
     * 显式校验（越界写会直接踩坏相邻状态） */
    if ((map == NULL) || (map->instance >= (uint8_t)(sizeof(s_hrpwm_instances)
                                                    / sizeof(s_hrpwm_instances[0])))
        || !hrpwm_is_valid_duty(duty)) {
        return -1;
    }

    inst_state = &s_hrpwm_instances[map->instance];

    if (!inst_state->channels[map->pwm_index].configured) {
        return -1;
    }

    if (hrpwm_phase_targets_channel(map->instance, map->pwm_index)) {
        return -1;
    }

    inst_state->channels[map->pwm_index].duty = duty;

    if (hrpwm_apply_duty(map) != 0) {
        return -1;
    }

    if (hrpwm_phase_refs_channel(map->instance, map->pwm_index)) {
        return hrpwm_apply_phase(map->instance);
    }

    return 0;
}

/**
 * @brief 直接设置占空比（不做移相联动）
 * @param ch 逻辑通道号
 * @param duty 占空比
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
ATTR_RAMFUNC
static int hrpwm_set_duty_direct(intf_hrpwm_ch_t ch, float duty) {
    const hrpwm_channel_map_t* map;
    hrpwm_instance_state_t* inst_state;

    map = hrpwm_get_channel_map(ch);
    if ((map == NULL) || !hrpwm_is_valid_duty(duty)) {
        return -1;
    }

    inst_state = &s_hrpwm_instances[map->instance];

    if (!inst_state->channels[map->pwm_index].configured) {
        return -1;
    }

    inst_state->channels[map->pwm_index].duty = duty;
    return hrpwm_apply_duty(map);
}

/**
 * @brief 同实例内一次性直接更新两路占空比（合并影子寄存器解锁）
 * @param ch_a 通道 A
 * @param duty_a 占空比 A
 * @param ch_b 通道 B
 * @param duty_b 占空比 B
 * @return 0 = 成功；-1 = 参数非法或跨实例
 */
ATTR_RAMFUNC
static int hrpwm_set_duty_direct_dual(
    intf_hrpwm_ch_t ch_a, float duty_a,
    intf_hrpwm_ch_t ch_b, float duty_b)
{
    const hrpwm_channel_map_t* map_a;
    const hrpwm_channel_map_t* map_b;
    PWM_Type* base;
    uint32_t full_reload;
    hrpwm_cmp_pair_t cmp_a, cmp_b;

    map_a = hrpwm_get_channel_map(ch_a);
    map_b = hrpwm_get_channel_map(ch_b);
    if (map_a == NULL || map_b == NULL) {
        return -1;
    }
    if (map_a->instance != map_b->instance) {
        return -1;
    }
    if (!hrpwm_is_valid_duty(duty_a) || !hrpwm_is_valid_duty(duty_b)) {
        return -1;
    }

    hrpwm_instance_state_t* inst = &s_hrpwm_instances[map_a->instance];
    if (!inst->channels[map_a->pwm_index].configured
        || !inst->channels[map_b->pwm_index].configured) {
        return -1;
    }

    base = hrpwm_get_base(map_a->instance);
    if (base == NULL) {
        return -1;
    }

    full_reload = hrpwm_get_full_reload(map_a->instance);

    inst->channels[map_a->pwm_index].duty = duty_a;
    inst->channels[map_b->pwm_index].duty = duty_b;

    cmp_a = hrpwm_calc_cmp_pair(full_reload, duty_a,
                                inst->channels[map_a->pwm_index].align);
    cmp_b = hrpwm_calc_cmp_pair(full_reload, duty_b,
                                inst->channels[map_b->pwm_index].align);

    pwm_shadow_register_unlock(base);
    pwm_cmp_update_cmp_value(base, map_a->cmp_start_index,
                             cmp_a.cmp_begin & HRPWM_RELOAD_MAX_24BIT,
                             (uint16_t)((cmp_a.cmp_begin >> 24U) & 0x0FU));
    pwm_cmp_update_cmp_value(base, map_a->cmp_start_index + 1U,
                             cmp_a.cmp_end & HRPWM_RELOAD_MAX_24BIT,
                             (uint16_t)((cmp_a.cmp_end >> 24U) & 0x0FU));
    pwm_cmp_update_cmp_value(base, map_b->cmp_start_index,
                             cmp_b.cmp_begin & HRPWM_RELOAD_MAX_24BIT,
                             (uint16_t)((cmp_b.cmp_begin >> 24U) & 0x0FU));
    pwm_cmp_update_cmp_value(base, map_b->cmp_start_index + 1U,
                             cmp_b.cmp_end & HRPWM_RELOAD_MAX_24BIT,
                             (uint16_t)((cmp_b.cmp_end >> 24U) & 0x0FU));

    return 0;
}

/**
 * @brief 设置 PWM0 频率
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_set_frequency_pwm0(uint32_t frequency_hz) {
    return hrpwm_apply_frequency(0, frequency_hz);
}

/**
 * @brief 设置 PWM1 频率
 * @param frequency_hz 频率 [Hz]
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_set_frequency_pwm1(uint32_t frequency_hz) {
    return hrpwm_apply_frequency(1, frequency_hz);
}

/**
 * @brief 设置通道抖动比较值
 * @param ch 逻辑通道号
 * @param jitter_cmp 抖动比较值
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int hrpwm_set_jitter(intf_hrpwm_ch_t ch, uint8_t jitter_cmp) {
    const hrpwm_channel_map_t* map;
    PWM_Type* base;

    map = hrpwm_get_channel_map(ch);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    if (!s_hrpwm_instances[map->instance].channels[map->pwm_index].configured) {
        return -1;
    }

    pwm_shadow_register_unlock(base);
    pwm_cmp_update_jitter_value(base, map->cmp_start_index, jitter_cmp);
    pwm_cmp_update_jitter_value(base, map->cmp_start_index + 1U, jitter_cmp);
    s_hrpwm_instances[map->instance].channels[map->pwm_index].jitter_cmp = jitter_cmp;

    return hrpwm_apply_duty(map);
}

/**
 * @brief 启动通道：使能配对输出并启动计数器
 * @param ch 逻辑通道号
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int hrpwm_start(intf_hrpwm_ch_t ch) {
    const hrpwm_channel_map_t* map;
    PWM_Type* base;

    map = hrpwm_get_channel_map(ch);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    if (!s_hrpwm_instances[map->instance].channels[map->pwm_index].configured) {
        return -1;
    }

    pwm_enable_output(base, map->pwm_index);
    pwm_enable_output(base, map->pwm_index + 1U);
    pwm_start_counter(base);
    pwm_issue_shadow_register_lock_event(base);
    s_hrpwm_instances[map->instance].channels[map->pwm_index].started = true;
    return 0;
}

/* 仅启动计数器 (CEN)，不使能任何通道的物理输出。CEN 置位对 pwm_x->GCR 幂等，
 * 后续 hrpwm_start() 补充使能输出时无需关心是否已启动过计数器。 */
/**
 * @brief 仅启动计数器（不使能物理输出）
 * @param inst 实例号
 * @return 0 = 成功；-1 = 基地址无效
 */
static int hrpwm_start_counter_only_impl(uint8_t inst) {
    PWM_Type* base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }
    pwm_start_counter(base);
    pwm_issue_shadow_register_lock_event(base);
    return 0;
}

/**
 * @brief PWM0 仅启动计数器包装
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_start_counter_only_pwm0(void) { return hrpwm_start_counter_only_impl(0); }

/**
 * @brief PWM1 仅启动计数器包装
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_start_counter_only_pwm1(void) { return hrpwm_start_counter_only_impl(1); }

/**
 * @brief 读取 PWM0 重装载值（原始寄存器值）
 * @return 重装载值
 */
static uint32_t hrpwm_get_reload_pwm0(void) { return pwm_get_reload_val(HPM_PWM0); }

/**
 * @brief 读取 PWM1 重装载值（原始寄存器值）
 * @return 重装载值
 */
static uint32_t hrpwm_get_reload_pwm1(void) { return pwm_get_reload_val(HPM_PWM1); }

/**
 * @brief 读取 PWM0 比较器值（原始寄存器值）
 * @param cmp_index 比较器索引
 * @return 比较值
 */
static uint32_t hrpwm_get_cmp_value_pwm0(uint8_t cmp_index) {
    return pwm_cmp_get_cmp_value(HPM_PWM0, cmp_index);
}

/**
 * @brief 读取 PWM1 比较器值（原始寄存器值）
 * @param cmp_index 比较器索引
 * @return 比较值
 */
static uint32_t hrpwm_get_cmp_value_pwm1(uint8_t cmp_index) {
    return pwm_cmp_get_cmp_value(HPM_PWM1, cmp_index);
}

/**
 * @brief 停止通道：禁用配对输出
 * @param ch 逻辑通道号
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int hrpwm_stop(intf_hrpwm_ch_t ch) {
    const hrpwm_channel_map_t* map;
    PWM_Type* base;

    map = hrpwm_get_channel_map(ch);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    if (!s_hrpwm_instances[map->instance].channels[map->pwm_index].configured) {
        return -1;
    }

    pwm_disable_output(base, map->pwm_index);
    pwm_disable_output(base, map->pwm_index + 1U);
    s_hrpwm_instances[map->instance].channels[map->pwm_index].started = false;
    return 0;
}

/**
 * @brief 软件强制通道输出低电平
 * @param ch 逻辑通道号
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int hrpwm_force_low(intf_hrpwm_ch_t ch) {
    const hrpwm_channel_map_t* map;
    PWM_Type* base;

    map = hrpwm_get_channel_map(ch);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    if (!s_hrpwm_instances[map->instance].channels[map->pwm_index].configured) {
        return -1;
    }

    pwm_config_force_cmd_timing(base, pwm_force_immediately);
    pwm_enable_pwm_sw_force_output(base, map->pwm_index);
    pwm_enable_pwm_sw_force_output(base, map->pwm_index + 1U);
    pwm_set_force_output(
        base, PWM_FORCE_OUTPUT(map->pwm_index, pwm_output_0)
                  | PWM_FORCE_OUTPUT((map->pwm_index + 1U), pwm_output_0));
    s_hrpwm_instances[map->instance].force_mask |=
        (uint8_t)((1U << map->pwm_index) | (1U << (map->pwm_index + 1U)));
    pwm_enable_sw_force(base);
    return 0;
}

/**
 * @brief 解除软件强制输出
 * @param ch 逻辑通道号
 * @return 0 = 成功；-1 = 参数非法
 */
static int hrpwm_force_release(intf_hrpwm_ch_t ch) {
    const hrpwm_channel_map_t* map;
    PWM_Type* base;

    map = hrpwm_get_channel_map(ch);
    if (map == NULL) {
        return -1;
    }

    base = hrpwm_get_base(map->instance);
    if (base == NULL) {
        return -1;
    }

    pwm_disable_pwm_sw_force_output(base, map->pwm_index);
    pwm_disable_pwm_sw_force_output(base, map->pwm_index + 1U);
    s_hrpwm_instances[map->instance].force_mask &=
        (uint8_t)~((1U << map->pwm_index) | (1U << (map->pwm_index + 1U)));
    if (s_hrpwm_instances[map->instance].force_mask == 0U) {
        pwm_disable_sw_force(base);
    }
    return 0;
}

/**
 * @brief 配置故障保护（模式/恢复方式/触发源，应用到全部实例）
 * @param cfg 故障配置
 * @return 0 = 成功；-1 = 参数非法
 */
static int hrpwm_config_fault(const intf_hrpwm_fault_cfg_t* cfg) {
    pwm_fault_source_config_t fault_config = {0};
    pwm_fault_mode_t fault_mode;
    pwm_fault_recovery_trigger_t fault_recovery;

    if (cfg == NULL) {
        return -1;
    }

    switch (cfg->mode) {
    case INTF_HRPWM_FAULT_MODE_FORCE_LOW: fault_mode = pwm_fault_mode_force_output_0; break;
    case INTF_HRPWM_FAULT_MODE_FORCE_HIGH: fault_mode = pwm_fault_mode_force_output_1; break;
    case INTF_HRPWM_FAULT_MODE_HIGH_Z: fault_mode = pwm_fault_mode_force_output_highz; break;
    default: return -1;
    }

    switch (cfg->recovery) {
    case INTF_HRPWM_FAULT_RECOVERY_IMMEDIATELY:
        fault_recovery = pwm_fault_recovery_immediately;
        break;
    case INTF_HRPWM_FAULT_RECOVERY_ON_RELOAD: fault_recovery = pwm_fault_recovery_on_reload; break;
    case INTF_HRPWM_FAULT_RECOVERY_ON_HW_EVENT:
        fault_recovery = pwm_fault_recovery_on_hw_event;
        break;
    case INTF_HRPWM_FAULT_RECOVERY_ON_FAULT_CLEAR:
        fault_recovery = pwm_fault_recovery_on_fault_clear;
        break;
    default: return -1;
    }

    fault_config.fault_output_recovery_trigger = fault_recovery;
    switch (cfg->source) {
    case INTF_HRPWM_FAULT_SRC_INTERNAL_0:
        fault_config.source_mask = pwm_fault_source_internal_0;
        break;
    case INTF_HRPWM_FAULT_SRC_INTERNAL_1:
        fault_config.source_mask = pwm_fault_source_internal_1;
        break;
    case INTF_HRPWM_FAULT_SRC_INTERNAL_2:
        fault_config.source_mask = pwm_fault_source_internal_2;
        break;
    case INTF_HRPWM_FAULT_SRC_INTERNAL_3:
        fault_config.source_mask = pwm_fault_source_internal_3;
        break;
    case INTF_HRPWM_FAULT_SRC_EXTERNAL_0:
        fault_config.source_mask = pwm_fault_source_external_0;
        fault_config.fault_external_0_active_low = cfg->active_low;
        break;
    case INTF_HRPWM_FAULT_SRC_EXTERNAL_1:
        fault_config.source_mask = pwm_fault_source_external_1;
        fault_config.fault_external_1_active_low = cfg->active_low;
        break;
    case INTF_HRPWM_FAULT_SRC_DEBUG: fault_config.source_mask = pwm_fault_source_debug; break;
    default: return -1;
    }

    for (uint8_t inst = 0; inst < HRPWM_INSTANCE_COUNT; inst++) {
        PWM_Type* base = hrpwm_get_base(inst);

        if (base == NULL) {
            return -1;
        }

        for (uint8_t pwm_index = 0; pwm_index < PWM_SOC_PWM_MAX_COUNT; pwm_index++) {
            base->PWMCFG[pwm_index] =
                (base->PWMCFG[pwm_index]
                 & ~(PWM_PWMCFG_FAULTMODE_MASK | PWM_PWMCFG_FAULTRECTIME_MASK))
                | PWM_PWMCFG_FAULTMODE_SET(fault_mode)
                | PWM_PWMCFG_FAULTRECTIME_SET(fault_recovery);
        }

        pwm_config_fault_source(base, &fault_config);
        s_hrpwm_instances[inst].fault_configured = true;
    }

    return 0;
}

/**
 * @brief 清除全部实例的故障与状态标志
 * @return 0 = 成功
 */
static int hrpwm_clear_fault(void) {
    for (uint8_t inst = 0; inst < HRPWM_INSTANCE_COUNT; inst++) {
        PWM_Type* base = hrpwm_get_base(inst);
        pwm_clear_fault(base);
        pwm_clear_status(base, pwm_get_status(base));
    }
    return 0;
}

/* 中断回调函数指针数组 */
static intf_hrpwm_irq_callback_t s_hrpwm_reload_callback[HRPWM_INSTANCE_COUNT] = {NULL};

/* PWM0中断处理函数 */
#if defined(IRQn_PWM0)
SDK_DECLARE_EXT_ISR_M(IRQn_PWM0, isr_pwm0)
void isr_pwm0(void) {
    uint32_t status = pwm_get_status(BOARD_APP_HRPWM0);
    pwm_clear_status(BOARD_APP_HRPWM0, status);

    if ((status & PWM_IRQ_RELOAD) && (s_hrpwm_reload_callback[0] != NULL)) {
        s_hrpwm_reload_callback[0]();
    }
}
#endif

/* PWM1中断处理函数 */
#if defined(IRQn_PWM1)
SDK_DECLARE_EXT_ISR_M(IRQn_PWM1, isr_pwm1)
void isr_pwm1(void) {
    uint32_t status = pwm_get_status(BOARD_APP_HRPWM1);
    pwm_clear_status(BOARD_APP_HRPWM1, status);

    if ((status & PWM_IRQ_RELOAD) && (s_hrpwm_reload_callback[1] != NULL)) {
        s_hrpwm_reload_callback[1]();
    }
}
#endif

/* PWM0中断配置函数 */
/**
 * @brief PWM0 配置重载中断回调
 * @param callback 回调
 * @return 0 = 成功
 */
static int hrpwm_config_reload_irq_pwm0(intf_hrpwm_irq_callback_t callback) {
    s_hrpwm_reload_callback[0] = callback;
    return 0;
}

/**
 * @brief PWM0 使能重载中断
 * @return 0 = 成功；-1 = 基地址无效
 */
static int hrpwm_enable_reload_irq_pwm0(void) {
    PWM_Type* base = hrpwm_get_base(0);
    if (base == NULL) {
        return -1;
    }

    pwm_enable_irq(base, PWM_IRQ_RELOAD);
    intc_m_enable_irq_with_priority(IRQn_PWM0, 2);
    return 0;
}

/**
 * @brief PWM0 禁用重载中断
 * @return 0 = 成功；-1 = 基地址无效
 */
static int hrpwm_disable_reload_irq_pwm0(void) {
    PWM_Type* base = hrpwm_get_base(0);
    if (base == NULL) {
        return -1;
    }

    pwm_disable_irq(base, PWM_IRQ_RELOAD);
    return 0;
}

/* PWM1中断配置函数 */
/**
 * @brief PWM1 配置重载中断回调
 * @param callback 回调
 * @return 0 = 成功
 */
static int hrpwm_config_reload_irq_pwm1(intf_hrpwm_irq_callback_t callback) {
    s_hrpwm_reload_callback[1] = callback;
    return 0;
}

/**
 * @brief PWM1 使能重载中断
 * @return 0 = 成功；-1 = 基地址无效
 */
static int hrpwm_enable_reload_irq_pwm1(void) {
    PWM_Type* base = hrpwm_get_base(1);
    if (base == NULL) {
        return -1;
    }

    pwm_enable_irq(base, PWM_IRQ_RELOAD);
    intc_m_enable_irq_with_priority(IRQn_PWM1, 1);
    return 0;
}

/**
 * @brief PWM1 禁用重载中断
 * @return 0 = 成功；-1 = 基地址无效
 */
static int hrpwm_disable_reload_irq_pwm1(void) {
    PWM_Type* base = hrpwm_get_base(1);
    if (base == NULL) {
        return -1;
    }

    pwm_disable_irq(base, PWM_IRQ_RELOAD);
    return 0;
}

/* 移相功能实现 - 通过CMP值偏移实现同一PWM实例内不同pair之间的移相 */
/**
 * @brief 设置实例内两配对间的移相角
 * @param cfg 移相配置
 * @return 0 = 成功；-1 = 参数非法或未配置
 */
static int hrpwm_set_phase(const intf_hrpwm_phase_cfg_t* cfg) {
    uint8_t prev_target_pair;
    bool restore_prev_target = false;

    if (cfg == NULL || cfg->inst >= HRPWM_INSTANCE_COUNT) {
        return -1;
    }

    if (cfg->ref_pair > 1U || cfg->target_pair > 1U || cfg->ref_pair == cfg->target_pair) {
        return -1;
    }

    hrpwm_instance_state_t* inst_state = &s_hrpwm_instances[cfg->inst];
    float max_phase = inst_state->phase_limit.max_phase_deg;

    if (!isfinite(cfg->phase_deg) || !isfinite(max_phase) || cfg->phase_deg < 0.0f
        || cfg->phase_deg > max_phase) {
        return -1;
    }

    /* For a static initial phase (before either pair starts), keep support for
     * the full 0~180° configuration range. Once the waveform is already
     * running, add a guard band near 90° and degrade continuous phase updates
     * to the more stable 0~89° range to avoid the known boundary glitch near
     * the 90° wrapped-window transition. */
    if (hrpwm_phase_is_running(cfg->inst) && cfg->phase_deg > HRPWM_PHASE_RUNTIME_GUARD_MAX_DEG) {
        return -1;
    }

    uint32_t period_count = hrpwm_get_full_reload(cfg->inst) + 1U;
    uint32_t phase_count = (uint32_t)((float)period_count * cfg->phase_deg / 360.0f);

    if (phase_count >= period_count) {
        phase_count = period_count - 1U;
    }

    const hrpwm_channel_map_t* ref_map = hrpwm_get_pair_map(cfg->inst, cfg->ref_pair);
    const hrpwm_channel_map_t* target_map = hrpwm_get_pair_map(cfg->inst, cfg->target_pair);

    if (ref_map == NULL || target_map == NULL) {
        return -1;
    }

    hrpwm_channel_state_t* ref_ch = &inst_state->channels[ref_map->pwm_index];
    hrpwm_channel_state_t* target_ch = &inst_state->channels[target_map->pwm_index];

    if (!ref_ch->configured || !target_ch->configured) {
        return -1;
    }

    if (ref_ch->duty > inst_state->phase_limit.max_duty_ref
        || target_ch->duty > inst_state->phase_limit.max_duty_target) {
        return -1;
    }

    prev_target_pair = inst_state->phase.target_pair;
    if (inst_state->phase.active && prev_target_pair != cfg->target_pair) {
        restore_prev_target = true;
    }

    inst_state->phase.active = true;
    inst_state->phase.ref_pair = cfg->ref_pair;
    inst_state->phase.target_pair = cfg->target_pair;
    inst_state->phase.phase_deg = cfg->phase_deg;
    inst_state->phase.phase_count = phase_count;

    if (restore_prev_target && hrpwm_restore_pair_waveform(cfg->inst, prev_target_pair) != 0) {
        return -1;
    }

    return hrpwm_apply_phase(cfg->inst);
}

/**
 * @brief 配置移相限值（应用到全部实例）
 * @param limit 移相限值
 * @return 0 = 成功；-1 = 参数非法
 */
static int hrpwm_config_phase_limit(const intf_hrpwm_phase_limit_t* limit) {
    if (limit == NULL) {
        return -1;
    }

    if (!isfinite(limit->max_phase_deg) || !isfinite(limit->max_duty_ref)
        || !isfinite(limit->max_duty_target) || limit->max_phase_deg < 0.0f
        || limit->max_duty_ref < 0.0f || limit->max_duty_ref > 1.0f || limit->max_duty_target < 0.0f
        || limit->max_duty_target > 1.0f) {
        return -1;
    }

    for (uint8_t inst = 0; inst < HRPWM_INSTANCE_COUNT; inst++) {
        s_hrpwm_instances[inst].phase_limit.max_phase_deg = limit->max_phase_deg;
        s_hrpwm_instances[inst].phase_limit.max_duty_ref = limit->max_duty_ref;
        s_hrpwm_instances[inst].phase_limit.max_duty_target = limit->max_duty_target;
    }

    return 0;
}

/* 触发比较器：比较值 = 计数谷底（回卷点）+ delay_ns。
 * tick = f_pwm_clk × delay_ns / 1e9 —— 只依赖时钟，与 PWM 频率解耦。 */
/**
 * @brief 延时（ns）转换为 PWM 时钟 tick
 * @param inst 实例号
 * @param delay_ns 延时 [ns]
 * @return tick 数
 */
static uint32_t hrpwm_delay_ns_to_ticks(uint8_t inst, uint32_t delay_ns) {
    uint32_t clock_hz = clock_get_frequency(s_hrpwm_instances[inst].clock_name);

    return (uint32_t)(((uint64_t) clock_hz * (uint64_t) delay_ns) / 1000000000ULL);
}

/**
 * @brief 配置触发比较器与窄脉冲输出（CHxREF → TRGM）
 * @param inst 实例号
 * @param cmp_index CMP 索引
 * @param delay_ns 相对计数谷底的延时 [ns]
 * @return 0 = 成功；-1 = 参数非法或超周期
 */
static int hrpwm_config_trigger_cmp_impl(uint8_t inst, uint8_t cmp_index, uint32_t delay_ns) {
    PWM_Type* base;
    uint32_t ticks;
    uint32_t reload;
    pwm_cmp_config_t cmp_cfg;
    pwm_output_channel_t out_cfg;

    if (cmp_index >= PWM_SOC_CMP_MAX_COUNT) {
        return -1;
    }

    base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }

    ticks = hrpwm_delay_ns_to_ticks(inst, delay_ns);
    reload = hrpwm_get_full_reload(inst);
    if ((reload != 0U) && (ticks >= reload)) {
        return -1; /* 触发点必须落在周期内（reload 尚未配置时跳过校验） */
    }

    memset(&cmp_cfg, 0, sizeof(cmp_cfg));
    cmp_cfg.enable_ex_cmp = false;
    cmp_cfg.mode = pwm_cmp_mode_output_compare;
    cmp_cfg.update_trigger = pwm_shadow_register_update_on_modify;
    cmp_cfg.cmp = ticks;
    pwm_config_cmp(base, cmp_index, &cmp_cfg);

    /* 输出通道 start==end==cmp_index：匹配处产生窄脉冲（CHxREF → TRGM） */
    out_cfg.cmp_start_index = cmp_index;
    out_cfg.cmp_end_index = cmp_index;
    out_cfg.invert_output = false;
    pwm_config_output_channel(base, cmp_index, &out_cfg);

    pwm_issue_shadow_register_lock_event(base);
    return 0;
}

/**
 * @brief 直接写 CMP 工作寄存器设置触发延时（调试期扫描用）
 * @param inst 实例号
 * @param cmp_index CMP 索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 参数非法
 */
ATTR_RAMFUNC
static int hrpwm_set_trigger_cmp_delay_impl(uint8_t inst, uint8_t cmp_index, uint32_t delay_ns) {
    PWM_Type* base;
    uint32_t ticks;

    if (cmp_index >= PWM_SOC_CMP_MAX_COUNT) {
        return -1;
    }

    base = hrpwm_get_base(inst);
    if (base == NULL) {
        return -1;
    }

    ticks = hrpwm_delay_ns_to_ticks(inst, delay_ns);

    /* 直接写 CMP 工作寄存器（update_trigger=on_modify，无需 SHLK 握手），
     * 单次存储，供调试期扫描触发点使用。 */
    base->CMP[cmp_index] = PWM_CMP_CMP_SET(ticks & 0xFFFFFFu);
    return 0;
}

/**
 * @brief PWM0 配置触发比较器包装
 * @param cmp_index CMP 索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_config_trigger_cmp_pwm0(uint8_t cmp_index, uint32_t delay_ns) {
    return hrpwm_config_trigger_cmp_impl(0, cmp_index, delay_ns);
}

/**
 * @brief PWM1 配置触发比较器包装
 * @param cmp_index CMP 索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
static int hrpwm_config_trigger_cmp_pwm1(uint8_t cmp_index, uint32_t delay_ns) {
    return hrpwm_config_trigger_cmp_impl(1, cmp_index, delay_ns);
}

/**
 * @brief PWM0 设置触发延时装包
 * @param cmp_index CMP 索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
ATTR_RAMFUNC
static int hrpwm_set_trigger_cmp_delay_pwm0(uint8_t cmp_index, uint32_t delay_ns) {
    return hrpwm_set_trigger_cmp_delay_impl(0, cmp_index, delay_ns);
}

/**
 * @brief PWM1 设置触发延时装包
 * @param cmp_index CMP 索引
 * @param delay_ns 延时 [ns]
 * @return 0 = 成功；-1 = 失败
 */
ATTR_RAMFUNC
static int hrpwm_set_trigger_cmp_delay_pwm1(uint8_t cmp_index, uint32_t delay_ns) {
    return hrpwm_set_trigger_cmp_delay_impl(1, cmp_index, delay_ns);
}

static const intf_hrpwm_t s_hrpwm_ops_pwm0 = {
    .instance_id = 0,
    .init_pair = hrpwm_init_pair,
    .set_duty = hrpwm_set_duty,
    .set_duty_direct = hrpwm_set_duty_direct,
    .set_duty_direct_dual = hrpwm_set_duty_direct_dual,
    .set_frequency = hrpwm_set_frequency_pwm0,
    .set_jitter = hrpwm_set_jitter,
    .start = hrpwm_start,
    .stop = hrpwm_stop,
    .force_low = hrpwm_force_low,
    .force_release = hrpwm_force_release,
    .config_fault = hrpwm_config_fault,
    .clear_fault = hrpwm_clear_fault,
    .config_reload_irq = hrpwm_config_reload_irq_pwm0,
    .enable_reload_irq = hrpwm_enable_reload_irq_pwm0,
    .disable_reload_irq = hrpwm_disable_reload_irq_pwm0,
    .set_phase = hrpwm_set_phase,
    .config_phase_limit = hrpwm_config_phase_limit,
    .config_trigger_cmp = hrpwm_config_trigger_cmp_pwm0,
    .set_trigger_cmp_delay = hrpwm_set_trigger_cmp_delay_pwm0,
    .start_counter_only = hrpwm_start_counter_only_pwm0,
    .get_reload = hrpwm_get_reload_pwm0,
    .get_cmp_value = hrpwm_get_cmp_value_pwm0,
};

static const intf_hrpwm_t s_hrpwm_ops_pwm1 = {
    .instance_id = 1,
    .init_pair = hrpwm_init_pair,
    .set_duty = hrpwm_set_duty,
    .set_duty_direct = hrpwm_set_duty_direct,
    .set_duty_direct_dual = hrpwm_set_duty_direct_dual,
    .set_frequency = hrpwm_set_frequency_pwm1,
    .set_jitter = hrpwm_set_jitter,
    .start = hrpwm_start,
    .stop = hrpwm_stop,
    .force_low = hrpwm_force_low,
    .force_release = hrpwm_force_release,
    .config_fault = hrpwm_config_fault,
    .clear_fault = hrpwm_clear_fault,
    .config_reload_irq = hrpwm_config_reload_irq_pwm1,
    .enable_reload_irq = hrpwm_enable_reload_irq_pwm1,
    .disable_reload_irq = hrpwm_disable_reload_irq_pwm1,
    .set_phase = hrpwm_set_phase,
    .config_phase_limit = hrpwm_config_phase_limit,
    .config_trigger_cmp = hrpwm_config_trigger_cmp_pwm1,
    .set_trigger_cmp_delay = hrpwm_set_trigger_cmp_delay_pwm1,
    .start_counter_only = hrpwm_start_counter_only_pwm1,
    .get_reload = hrpwm_get_reload_pwm1,
    .get_cmp_value = hrpwm_get_cmp_value_pwm1,
};

void hpm_hrpwm_driver_register(void) {
    hrpwm_init_instances();
    intf_hrpwm_register(&s_hrpwm_ops_pwm0);
    intf_hrpwm_register(&s_hrpwm_ops_pwm1);
}
