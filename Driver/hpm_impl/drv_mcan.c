/**
 * @file    drv_mcan.c
 * @brief   MCAN 驱动 - HPM MCAN 硬件实现
 * @author  Kaiser
 *
 * 最大化复用 HPM SDK 已有代码:
 * - mcan_get_default_config() → 填充完整默认配置
 * - mcan_init() → 硬件初始化
 * - mcan_transmit_blocking() / mcan_receive_from_fifo_blocking() → 收发
 * - mcan_parse_protocol_status() / mcan_get_error_counter() → 状态查询
 * - mcan_set_filter_element() → 过滤器配置
 * - 复用 SDK demo 中的 can_info_t[] + ISR 模式
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_can.h"
#include "intf_clock.h"
#include "board.h"

#include "hpm_mcan_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_csr_drv.h"
#include "hpm_interrupt.h"
#include "hpm_soc_irq.h"

#include <stddef.h>
#include <string.h>

/* ============================================================================
 * MCAN 实例数量 (使用 SDK 定义的 MCAN_SOC_MAX_COUNT)
 * ============================================================================ */

#define DRV_MCAN_INSTANCE_COUNT MCAN_SOC_MAX_COUNT

/* ============================================================================
 * DLC 编码表: 字节数 → DLC 寄存器值 (CAN FD)
 * ============================================================================ */

static const uint8_t s_dlc_encode_table[65] = {
    [0]  = 0,   [1]  = 1,   [2]  = 2,   [3]  = 3,
    [4]  = 4,   [5]  = 5,   [6]  = 6,   [7]  = 7,
    [8]  = 8,   [12] = 9,   [16] = 10,  [20] = 11,
    [24] = 12,  [32] = 13,  [48] = 14,  [64] = 15,
};

/**
 * @brief 字节数编码为 CAN FD DLC 寄存器值
 * @param byte_count 字节数
 * @return DLC 值
 */
static uint8_t can_dlc_encode(uint8_t byte_count)
{
    if (byte_count <= 8) {
        return byte_count;
    }
    if (byte_count <= 64) {
        return s_dlc_encode_table[byte_count];
    }
    return 15;
}

/* ============================================================================
 * AHB RAM 消息缓冲区 (硬件要求: 必须放在 AHB SRAM 段)
 * 复用 SDK demo 模式: ATTR_PLACE_AT(".ahb_sram")
 * ============================================================================ */

#if defined(HPM_MCAN0)
ATTR_PLACE_AT(".ahb_sram") static uint32_t mcan0_msg_buf[MCAN_MSG_BUF_SIZE_IN_WORDS];
#endif
#if defined(HPM_MCAN1)
ATTR_PLACE_AT(".ahb_sram") static uint32_t mcan1_msg_buf[MCAN_MSG_BUF_SIZE_IN_WORDS];
#endif
#if defined(HPM_MCAN2)
ATTR_PLACE_AT(".ahb_sram") static uint32_t mcan2_msg_buf[MCAN_MSG_BUF_SIZE_IN_WORDS];
#endif
#if defined(HPM_MCAN3)
ATTR_PLACE_AT(".ahb_sram") static uint32_t mcan3_msg_buf[MCAN_MSG_BUF_SIZE_IN_WORDS];
#endif

/* ============================================================================
 * 实例管理结构体 (复用 SDK demo 的 can_info_t 模式)
 * ============================================================================ */

/**
 * @brief MCAN 实例管理结构
 */
typedef struct {
    MCAN_Type    *base;                /**< MCAN 寄存器基地址 */
    clock_name_t  clock_name;          /**< 外设时钟 */
    uint32_t      irq_num;             /**< 中断号 */
    uint32_t      ram_base;            /**< 消息 RAM 基地址 */
    uint32_t      ram_size;            /**< 消息 RAM 大小 */
    uint8_t       std_filter_capacity; /**< 标准过滤器容量 */
    uint8_t       ext_filter_capacity; /**< 扩展过滤器容量 */
    bool          initialized;         /**< 是否已初始化 */
    bool          canfd_enabled;       /**< 是否启用 CAN FD */
    uint32_t      interrupt_mask;      /**< 事件中断掩码 */
    intf_can_irq_callback_t irq_cb;    /**< 中断回调 */
    void         *irq_user_data;       /**< 中断回调用户数据 */
} mcan_instance_t;

/**
 * @brief 进入 CAN 临界区（保存并关闭全局中断）
 * @return 保存的中断状态
 */
static uint32_t drv_can_enter_critical(void)
{
    return read_clear_csr(CSR_MSTATUS, CSR_MSTATUS_MIE_MASK);
}

/**
 * @brief 退出 CAN 临界区（恢复全局中断）
 * @param irq_state 进入临界区时保存的中断状态
 */
static void drv_can_exit_critical(uint32_t irq_state)
{
    write_csr(CSR_MSTATUS, irq_state);
}

/*
 * timeout_ms 语义（send / receive 通用，与 drv_uart 一致）：
 *   0          = 不等待（单次尝试）
 *   UINT32_MAX = 无限等待
 *   其他       = 毫秒级超时
 */
/**
 * @brief 毫秒转 CPU cycle
 * @param ms 毫秒数
 * @return 对应 cycle 数
 */
static uint32_t mcan_ms_to_cycles(uint32_t ms)
{
    return (uint32_t)((uint64_t) ms * (intf_clock_get_cpu_freq() / 1000U));
}

/**
 * @brief 判断超时是否到达
 * @param start 起始 cycle
 * @param timeout_cycles 超时 cycle
 * @param timeout_ms 超时毫秒语义
 * @return true = 已超时
 */
static bool mcan_timeout_elapsed(uint32_t start, uint32_t timeout_cycles, uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return true; /* 不等待 */
    }
    if (timeout_ms == UINT32_MAX) {
        return false; /* 无限等待 */
    }
    return (uint32_t)(intf_clock_get_cycle() - start) >= timeout_cycles;
}

/**
 * @brief 校验 CAN 帧 ID 是否合法
 * @param frame CAN 帧
 * @return true = 合法
 */
static bool mcan_frame_id_is_valid(const intf_can_frame_t *frame)
{
    if (frame->is_ext_id) {
        return frame->id <= 0x1FFFFFFFU;
    }
    return frame->id <= 0x7FFU;
}

/**
 * @brief 校验 CAN 帧是否可发送（ID / 类型 / DLC）
 * @param inst 实例
 * @param frame CAN 帧
 * @return true = 合法
 */
static bool mcan_frame_is_valid(const mcan_instance_t *inst,
                                const intf_can_frame_t *frame)
{
    if (inst == NULL || frame == NULL) {
        return false;
    }
    if (!mcan_frame_id_is_valid(frame)) {
        return false;
    }
    if (frame->frame_type == INTF_CAN_FRAME_CLASSIC) {
        return frame->dlc <= 8U;
    }
    if (!inst->canfd_enabled) {
        return false;
    }

    switch (frame->dlc) {
    case 0U:
    case 1U:
    case 2U:
    case 3U:
    case 4U:
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 12U:
    case 16U:
    case 20U:
    case 24U:
    case 32U:
    case 48U:
    case 64U:
        return true;
    default:
        return false;
    }
}

static mcan_instance_t s_mcan_instances[DRV_MCAN_INSTANCE_COUNT];

static void mcan_deinit_impl(uint8_t inst_id);

/* ============================================================================
 * 获取实例基地址 (类似 drv_hrpwm.c 的 hrpwm_get_base)
 * ============================================================================ */

/**
 * @brief 获取实例基地址
 * @param inst_id 实例号
 * @return MCAN 基地址；越界返回 NULL
 */
static MCAN_Type *mcan_get_base(uint8_t inst_id)
{
    if (inst_id >= DRV_MCAN_INSTANCE_COUNT) {
        return NULL;
    }
    return s_mcan_instances[inst_id].base;
}

/**
 * @brief 获取实例结构指针
 * @param inst_id 实例号
 * @return 实例指针；越界返回 NULL
 */
static mcan_instance_t *mcan_get_instance(uint8_t inst_id)
{
    if (inst_id >= DRV_MCAN_INSTANCE_COUNT) {
        return NULL;
    }
    return &s_mcan_instances[inst_id];
}

/* ============================================================================
 * 事件掩码映射: intf_can_event_t → SDK MCAN_INT_* / MCAN_EVENT_*
 * ============================================================================ */

/**
 * @brief 接口事件掩码映射为 SDK 中断掩码
 * @param intf_events 接口事件掩码
 * @return SDK 中断掩码
 */
static uint32_t mcan_event_to_sdk_mask(uint32_t intf_events)
{
    uint32_t sdk_mask = 0U;

    if (intf_events & INTF_CAN_EVENT_RX_FIFO0_NEW_MSG)  sdk_mask |= MCAN_INT_RXFIFO0_NEW_MSG;
    if (intf_events & INTF_CAN_EVENT_RX_FIFO1_NEW_MSG)  sdk_mask |= MCAN_INT_RXFIFO1_NEW_MSG;
    if (intf_events & INTF_CAN_EVENT_RX_BUF_NEW_MSG)    sdk_mask |= MCAN_INT_MSG_STORE_TO_RXBUF;
    if (intf_events & INTF_CAN_EVENT_RX_FIFO0_FULL)     sdk_mask |= MCAN_INT_RXFIFO0_FULL;
    if (intf_events & INTF_CAN_EVENT_RX_FIFO1_FULL)     sdk_mask |= MCAN_INT_RXFIFO1_FULL;
    if (intf_events & INTF_CAN_EVENT_RX_FIFO0_MSG_LOST) sdk_mask |= MCAN_INT_RXFIFO0_MSG_LOST;
    if (intf_events & INTF_CAN_EVENT_RX_FIFO1_MSG_LOST) sdk_mask |= MCAN_INT_RXFIFO1_MSG_LOST;
    if (intf_events & INTF_CAN_EVENT_TX_COMPLETED)      sdk_mask |= MCAN_INT_TX_COMPLETED;
    if (intf_events & INTF_CAN_EVENT_TX_FIFO_EMPTY)     sdk_mask |= MCAN_INT_TXFIFO_EMPTY;
    if (intf_events & INTF_CAN_EVENT_TX_CANCEL_DONE)    sdk_mask |= MCAN_INT_TX_CANCEL_FINISHED;
    if (intf_events & INTF_CAN_EVENT_TX_EVT_FIFO_NEW)   sdk_mask |= MCAN_INT_TX_EVT_FIFO_NEW_ENTRY;
    if (intf_events & INTF_CAN_EVENT_TX_EVT_FIFO_FULL)  sdk_mask |= MCAN_INT_TX_EVT_FIFO_FULL;
    if (intf_events & INTF_CAN_EVENT_TX_EVT_FIFO_LOST)  sdk_mask |= MCAN_INT_TX_EVT_FIFO_EVT_LOST;
    if (intf_events & INTF_CAN_EVENT_BUS_OFF)           sdk_mask |= MCAN_INT_BUS_OFF_STATUS;
    if (intf_events & INTF_CAN_EVENT_ERROR_WARNING)     sdk_mask |= MCAN_INT_WARNING_STATUS;
    if (intf_events & INTF_CAN_EVENT_ERROR_PASSIVE)     sdk_mask |= MCAN_INT_ERROR_PASSIVE;
    if (intf_events & INTF_CAN_EVENT_PROTOCOL_ERROR) {
        sdk_mask |= (MCAN_INT_PROTOCOL_ERR_IN_DATA_PHASE |
                     MCAN_INT_PROTOCOL_ERR_IN_ARB_PHASE  |
                     MCAN_INT_BIT_ERROR_UNCORRECTED);
    }
    if (intf_events & INTF_CAN_EVENT_TIMEOUT)           sdk_mask |= MCAN_INT_TIMEOUT_OCCURRED;
    if (intf_events & INTF_CAN_EVENT_HIGH_PRIORITY_MSG) sdk_mask |= MCAN_INT_HIGH_PRIORITY_MSG;
    if (intf_events & INTF_CAN_EVENT_TIMESTAMP_WRAP)    sdk_mask |= MCAN_INT_TIMESTAMP_WRAPAROUND;
    if (intf_events & INTF_CAN_EVENT_RAM_ACCESS_FAIL)   sdk_mask |= MCAN_INT_MSG_RAM_ACCESS_FAILURE;

    return sdk_mask;
}

/**
 * @brief SDK 中断标志映射为接口事件掩码
 * @param sdk_flags SDK 中断标志
 * @return 接口事件掩码
 */
static uint32_t sdk_mask_to_mcan_event(uint32_t sdk_flags)
{
    uint32_t intf_events = 0U;

    if (sdk_flags & MCAN_INT_RXFIFO0_NEW_MSG)  intf_events |= INTF_CAN_EVENT_RX_FIFO0_NEW_MSG;
    if (sdk_flags & MCAN_INT_RXFIFO1_NEW_MSG)  intf_events |= INTF_CAN_EVENT_RX_FIFO1_NEW_MSG;
    if (sdk_flags & MCAN_INT_MSG_STORE_TO_RXBUF) intf_events |= INTF_CAN_EVENT_RX_BUF_NEW_MSG;
    if (sdk_flags & MCAN_INT_RXFIFO0_FULL)     intf_events |= INTF_CAN_EVENT_RX_FIFO0_FULL;
    if (sdk_flags & MCAN_INT_RXFIFO1_FULL)     intf_events |= INTF_CAN_EVENT_RX_FIFO1_FULL;
    if (sdk_flags & MCAN_INT_RXFIFO0_MSG_LOST) intf_events |= INTF_CAN_EVENT_RX_FIFO0_MSG_LOST;
    if (sdk_flags & MCAN_INT_RXFIFO1_MSG_LOST) intf_events |= INTF_CAN_EVENT_RX_FIFO1_MSG_LOST;
    if (sdk_flags & MCAN_INT_TX_COMPLETED)     intf_events |= INTF_CAN_EVENT_TX_COMPLETED;
    if (sdk_flags & MCAN_INT_TXFIFO_EMPTY)     intf_events |= INTF_CAN_EVENT_TX_FIFO_EMPTY;
    if (sdk_flags & MCAN_INT_TX_CANCEL_FINISHED) intf_events |= INTF_CAN_EVENT_TX_CANCEL_DONE;
    if (sdk_flags & MCAN_INT_TX_EVT_FIFO_NEW_ENTRY) intf_events |= INTF_CAN_EVENT_TX_EVT_FIFO_NEW;
    if (sdk_flags & MCAN_INT_TX_EVT_FIFO_FULL)   intf_events |= INTF_CAN_EVENT_TX_EVT_FIFO_FULL;
    if (sdk_flags & MCAN_INT_TX_EVT_FIFO_EVT_LOST) intf_events |= INTF_CAN_EVENT_TX_EVT_FIFO_LOST;
    if (sdk_flags & MCAN_INT_BUS_OFF_STATUS)     intf_events |= INTF_CAN_EVENT_BUS_OFF;
    if (sdk_flags & MCAN_INT_WARNING_STATUS)     intf_events |= INTF_CAN_EVENT_ERROR_WARNING;
    if (sdk_flags & MCAN_INT_ERROR_PASSIVE)      intf_events |= INTF_CAN_EVENT_ERROR_PASSIVE;
    if (sdk_flags & (MCAN_INT_PROTOCOL_ERR_IN_DATA_PHASE |
                     MCAN_INT_PROTOCOL_ERR_IN_ARB_PHASE  |
                     MCAN_INT_BIT_ERROR_UNCORRECTED))
        intf_events |= INTF_CAN_EVENT_PROTOCOL_ERROR;
    if (sdk_flags & MCAN_INT_TIMEOUT_OCCURRED)   intf_events |= INTF_CAN_EVENT_TIMEOUT;
    if (sdk_flags & MCAN_INT_HIGH_PRIORITY_MSG)  intf_events |= INTF_CAN_EVENT_HIGH_PRIORITY_MSG;
    if (sdk_flags & MCAN_INT_TIMESTAMP_WRAPAROUND) intf_events |= INTF_CAN_EVENT_TIMESTAMP_WRAP;
    if (sdk_flags & MCAN_INT_MSG_RAM_ACCESS_FAILURE) intf_events |= INTF_CAN_EVENT_RAM_ACCESS_FAIL;

    return intf_events;
}

/* ============================================================================
 * 帧映射: intf_can_frame_t → mcan_tx_frame_t
 * ============================================================================ */

/**
 * @brief 接口帧映射为 SDK 发送帧
 * @param src 接口帧
 * @param dst SDK 发送帧输出
 */
static void frame_to_sdk_tx(const intf_can_frame_t *src, mcan_tx_frame_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    if (src->is_ext_id) {
        dst->use_ext_id = 1;
        dst->ext_id = src->id & 0x1FFFFFFFU;
    } else {
        dst->std_id = src->id & 0x7FFU;
    }
    dst->rtr = src->is_remote ? 1U : 0U;
    dst->dlc = can_dlc_encode(src->dlc);
    dst->canfd_frame = (src->frame_type != INTF_CAN_FRAME_CLASSIC) ? 1U : 0U;
    dst->bitrate_switch = (src->frame_type == INTF_CAN_FRAME_FD_BRS) ? 1U : 0U;
    dst->message_marker_h = (uint8_t)((src->message_marker >> 8) & 0xFFU);
    dst->message_marker_l = (uint8_t)(src->message_marker & 0xFFU);
    memcpy(dst->data_8, src->data, src->dlc);
}

/* ============================================================================
 * 帧映射: mcan_rx_message_t → intf_can_frame_t
 * ============================================================================ */

/**
 * @brief SDK 接收消息映射为接口帧
 * @param src SDK 接收消息
 * @param dst 接口帧输出
 */
static void sdk_rx_to_frame(const mcan_rx_message_t *src, intf_can_frame_t *dst)
{
    uint8_t payload_size;

    memset(dst, 0, sizeof(*dst));

    dst->id = src->use_ext_id ? src->ext_id : src->std_id;
    dst->is_ext_id = (src->use_ext_id != 0U);
    dst->is_remote = (src->rtr != 0U);
    if (src->canfd_frame) {
        dst->frame_type = src->bitrate_switch
                          ? INTF_CAN_FRAME_FD_BRS
                          : INTF_CAN_FRAME_FD_NO_BRS;
    } else {
        dst->frame_type = INTF_CAN_FRAME_CLASSIC;
    }
    payload_size = (uint8_t)mcan_get_message_size_from_dlc(src->dlc);
    if (payload_size > sizeof(dst->data)) {
        payload_size = sizeof(dst->data);
    }
    dst->dlc = payload_size;
    dst->timestamp = src->rx_timestamp;
    dst->filter_index = src->filter_index;
    memcpy(dst->data, src->data_8, dst->dlc);
}

/* ============================================================================
 * 接口实现 — init
 * ============================================================================ */

/**
 * @brief 接口过滤器元素映射为 SDK 过滤器元素
 * @param src 接口过滤器元素
 * @param dst SDK 过滤器元素输出
 */
static void filter_to_sdk(const intf_can_filter_elem_t *src, mcan_filter_elem_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    dst->can_id_type = src->is_ext_id ? MCAN_CAN_ID_TYPE_EXTENDED
                                      : MCAN_CAN_ID_TYPE_STANDARD;

    if (src->type == INTF_CAN_FILTER_STORE_TO_BUF) {
        dst->filter_type = MCAN_FILTER_TYPE_CLASSIC_FILTER;
        dst->filter_config = MCAN_FILTER_ELEM_CFG_STORE_INTO_RX_BUFFER_OR_AS_DBG_MSG;
        dst->match_id = src->id;
        dst->offset = src->rxbuf_idx;
        dst->filter_event = 0U;
        dst->store_location = 0U;
    } else {
        dst->filter_type = (uint8_t)src->type;
        dst->filter_config = (src->target_fifo == INTF_CAN_FILTER_FIFO1)
                           ? MCAN_FILTER_ELEM_CFG_STORE_IN_RX_FIFO1_IF_MATCH
                           : MCAN_FILTER_ELEM_CFG_STORE_IN_RX_FIFO0_IF_MATCH;
        switch (src->type) {
        case INTF_CAN_FILTER_RANGE:
            dst->start_id = src->id;
            dst->end_id = src->mask;
            break;
        case INTF_CAN_FILTER_DUAL_ID:
            dst->id1 = src->id;
            dst->id2 = src->mask;
            break;
        case INTF_CAN_FILTER_CLASSIC:
        default:
            dst->filter_id = src->id;
            dst->filter_mask = src->mask;
            break;
        }
    }
}

/* ============================================================================
 * 接口实现 — init
 * ============================================================================ */

/**
 * @brief 初始化 MCAN 实例
 * @param inst_id 实例号
 * @param cfg CAN 配置
 * @return 0 = 成功；-1 = 参数非法或 SDK 初始化失败
 */
static int mcan_init_impl(uint8_t inst_id, const intf_can_cfg_t *cfg)
{
    mcan_instance_t *inst = &s_mcan_instances[inst_id];

    if (inst->base == NULL || cfg == NULL) {
        return -1;
    }
    if (cfg->baudrate == 0U) {
        return -1;
    }
    if (cfg->enable_canfd && cfg->baudrate_fd == 0U) {
        return -1;
    }

    if (inst->initialized) {
        mcan_deinit_impl(inst_id);
    }

    mcan_msg_buf_attr_t attr;
    attr.ram_base = inst->ram_base;
    attr.ram_size = inst->ram_size;
    hpm_stat_t status = mcan_set_msg_buf_attr(inst->base, &attr);
    if (status != status_success) {
        return -1;
    }

    mcan_config_t sdk_cfg;
    mcan_ram_config_t ram;
    mcan_get_default_config(inst->base, &sdk_cfg);
    mcan_get_default_ram_config(inst->base, &ram, cfg->enable_canfd);

    sdk_cfg.baudrate = cfg->baudrate;
    sdk_cfg.mode     = (mcan_node_mode_t)cfg->mode;
    sdk_cfg.enable_canfd = cfg->enable_canfd;
    if (cfg->enable_canfd) {
        sdk_cfg.baudrate_fd = cfg->baudrate_fd;
    }
    if (cfg->sample_point > 0U) {
        sdk_cfg.can20_samplepoint_min = cfg->sample_point;
        sdk_cfg.can20_samplepoint_max = cfg->sample_point;
    }
    if (cfg->sample_point_fd > 0U) {
        sdk_cfg.canfd_samplepoint_min = cfg->sample_point_fd;
        sdk_cfg.canfd_samplepoint_max = cfg->sample_point_fd;
    }
    sdk_cfg.disable_auto_retransmission = cfg->disable_auto_retransmission;
    sdk_cfg.enable_restricted_operation_mode = cfg->enable_restricted_mode;

    bool has_custom_ram = (cfg->ram.std_filter_count > 0U)
                       || (cfg->ram.ext_filter_count > 0U)
                       || (cfg->ram.rxfifo0_count > 0U)
                       || (cfg->ram.rxfifo1_count > 0U)
                       || (cfg->ram.rxbuf_count > 0U)
                       || (cfg->ram.txbuf_count > 0U)
                       || (cfg->ram.tx_evt_fifo_count > 0U);
    if (has_custom_ram) {
        if (cfg->ram.std_filter_count > 0U) ram.std_filter_elem_count = cfg->ram.std_filter_count;
        if (cfg->ram.ext_filter_count > 0U) ram.ext_filter_elem_count  = cfg->ram.ext_filter_count;
        if (cfg->ram.rxfifo0_count > 0U)    ram.rxfifos[0].elem_count  = cfg->ram.rxfifo0_count;
        if (cfg->ram.rxfifo1_count > 0U)    ram.rxfifos[1].elem_count  = cfg->ram.rxfifo1_count;
        if (cfg->ram.rxbuf_count > 0U)      ram.rxbuf_elem_count       = cfg->ram.rxbuf_count;
        if (cfg->ram.txbuf_count > 0U)      ram.txbuf_fifo_or_queue_elem_count = cfg->ram.txbuf_count;
        if (cfg->ram.tx_evt_fifo_count > 0U) ram.tx_evt_fifo_elem_count = cfg->ram.tx_evt_fifo_count;
        sdk_cfg.ram_config = ram;
    }

    sdk_cfg.interrupt_mask = mcan_event_to_sdk_mask(cfg->interrupt_mask);

    /* IR.TC（发送完成）需 TXBTIE 使能，否则 TC 中断不会产生（默认 0） */
    if ((cfg->interrupt_mask & INTF_CAN_EVENT_TX_COMPLETED) != 0U) {
        sdk_cfg.txbuf_trans_interrupt_mask = ~0UL;
    }

    /* 自管时钟：确保外设时钟已加入组 0（幂等）。源/分频为板级策略，见 drv_clock。 */
    clock_add_to_group(inst->clock_name, 0);

    uint32_t clk_freq = clock_get_frequency(inst->clock_name);
    status = mcan_init(inst->base, &sdk_cfg, clk_freq);
    if (status != status_success) {
        return -1;
    }

    inst->initialized = true;
    inst->std_filter_capacity = ram.std_filter_elem_count;
    inst->ext_filter_capacity = ram.ext_filter_elem_count;
    inst->canfd_enabled = cfg->enable_canfd;
    inst->interrupt_mask = cfg->interrupt_mask;
    return 0;
}

/* ============================================================================
 * 接口实现 — deinit
 * ============================================================================ */

/**
 * @brief 反初始化 MCAN 实例
 * @param inst_id 实例号
 */
static void mcan_deinit_impl(uint8_t inst_id)
{
    mcan_instance_t *inst;
    uint32_t flags;

    if (inst_id >= DRV_MCAN_INSTANCE_COUNT) {
        return;
    }

    inst = &s_mcan_instances[inst_id];

    if (inst->base == NULL) {
        return;
    }

    mcan_disable_interrupts(inst->base, UINT32_MAX);
    intc_m_disable_irq(inst->irq_num);
    flags = mcan_get_interrupt_flags(inst->base);
    if (flags != 0U) {
        mcan_clear_interrupt_flags(inst->base, flags);
    }

    mcan_deinit(inst->base);
    inst->std_filter_capacity = 0U;
    inst->ext_filter_capacity = 0U;
    inst->initialized = false;
    inst->canfd_enabled = false;
    inst->interrupt_mask = 0U;
    inst->irq_cb = NULL;
    inst->irq_user_data = NULL;
}

/* ============================================================================
 * 接口实现 — send (阻塞)
 * ============================================================================ */

/**
 * @brief 阻塞发送 CAN 帧（写入 TX FIFO）
 * @param inst_id 实例号
 * @param frame CAN 帧
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法或超时
 */
static int mcan_send_impl(uint8_t inst_id, const intf_can_frame_t *frame,
                          uint32_t timeout_ms)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || !mcan_frame_is_valid(inst, frame)) {
        return -1;
    }

    mcan_tx_frame_t tx;
    frame_to_sdk_tx(frame, &tx);

    uint32_t start = intf_clock_get_cycle();
    uint32_t timeout_cycles = mcan_ms_to_cycles(timeout_ms);

    /*
     * 写入 TX FIFO；FIFO 满时按 timeout_ms 等待空位，成功入队即返回。
     * 返回 0 表示帧已进入发送队列（不代表已上总线）；
     * 发送完成事件见 INTF_CAN_EVENT_TX_COMPLETED。
     */
    for (;;) {
        uint32_t put_index = 0;
        hpm_stat_t status = mcan_transmit_via_txfifo_nonblocking(base, &tx, &put_index);

        if (status == status_success) {
            return 0;
        }
        if (mcan_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            return -1;
        }
    }
}

/* ============================================================================
 * 接口实现 — send_nonblocking
 * ============================================================================ */

/**
 * @brief 非阻塞发送 CAN 帧
 * @param inst_id 实例号
 * @param frame CAN 帧
 * @param fifo_idx 发送 FIFO 索引输出
 * @return 0 = 成功；-1 = 参数非法或 FIFO 满
 */
static int mcan_send_nonblocking_impl(uint8_t inst_id,
                                       const intf_can_frame_t *frame,
                                       uint8_t *fifo_idx)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || !mcan_frame_is_valid(inst, frame)) {
        return -1;
    }

    mcan_tx_frame_t tx;
    frame_to_sdk_tx(frame, &tx);

    uint32_t idx = 0;
    hpm_stat_t status = mcan_transmit_via_txfifo_nonblocking(base, &tx, &idx);
    if (status == status_success && fifo_idx != NULL) {
        *fifo_idx = (uint8_t)idx;
    }
    return (status == status_success) ? 0 : -1;
}

/* ============================================================================
 * 接口实现 — send_add_request
 * ============================================================================ */

/**
 * @brief 请求发送指定 FIFO 中的报文
 * @param inst_id 实例号
 * @param fifo_idx 发送 FIFO 索引
 * @return 0 = 成功；-1 = 实例无效
 */
static int mcan_send_add_request_impl(uint8_t inst_id, uint8_t fifo_idx)
{
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL) {
        return -1;
    }
    mcan_send_add_request(base, (uint32_t)fifo_idx);
    return 0;
}

/* ============================================================================
 * 接口实现 — receive (阻塞)
 * ============================================================================ */

/**
 * @brief 阻塞接收 CAN 帧（轮询 RXFIFO0）
 * @param inst_id 实例号
 * @param frame CAN 帧输出
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法或超时
 */
static int mcan_receive_impl(uint8_t inst_id, intf_can_frame_t *frame,
                              uint32_t timeout_ms)
{
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || frame == NULL) {
        return -1;
    }

    uint32_t start = intf_clock_get_cycle();
    uint32_t timeout_cycles = mcan_ms_to_cycles(timeout_ms);

    /* 轮询 RXFIFO0；无数据时按 timeout_ms 等待 */
    for (;;) {
        if (mcan_get_rxfifo_fill_level(base, 0U) > 0U) {
            mcan_rx_message_t rx;
            memset(&rx, 0, sizeof(rx));
            if (mcan_read_rxfifo(base, 0U, &rx) != status_success) {
                return -1;
            }
            sdk_rx_to_frame(&rx, frame);
            return 0;
        }
        if (mcan_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            return -1;
        }
    }
}

/* ============================================================================
 * 接口实现 — receive_nonblocking
 * ============================================================================ */

/**
 * @brief 非阻塞接收 CAN 帧
 * @param inst_id 实例号
 * @param frame CAN 帧输出
 * @return 0 = 成功；-1 = 参数非法或无数据
 */
static int mcan_receive_nonblocking_impl(uint8_t inst_id,
                                          intf_can_frame_t *frame)
{
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || frame == NULL) {
        return -1;
    }

    mcan_rx_message_t rx;
    memset(&rx, 0, sizeof(rx));

    hpm_stat_t status = mcan_read_rxfifo(base, 0U, &rx);
    if (status != status_success) {
        return -1;
    }
    sdk_rx_to_frame(&rx, frame);
    return 0;
}

/* ============================================================================
 * 接口实现 — config_filter
 * ============================================================================ */

/**
 * @brief 配置过滤器（必要时临时进入配置模式）
 * @param inst_id 实例号
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 参数非法或 SDK 配置失败
 */
static int mcan_config_filter_impl(uint8_t inst_id, uint32_t index,
                                     const intf_can_filter_elem_t *elem)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || inst == NULL || elem == NULL) {
        return -1;
    }
    if (elem->is_ext_id) {
        if (elem->id > 0x1FFFFFFFU || elem->mask > 0x1FFFFFFFU) {
            return -1;
        }
        if (index >= inst->ext_filter_capacity) {
            return -1;
        }
    } else {
        if (elem->id > 0x7FFU || elem->mask > 0x7FFU) {
            return -1;
        }
        if (index >= inst->std_filter_capacity) {
            return -1;
        }
    }
    if (elem->type == INTF_CAN_FILTER_STORE_TO_BUF && elem->rxbuf_idx >= 64U) {
        return -1;
    }

    mcan_filter_elem_t sdk_elem;
    filter_to_sdk(elem, &sdk_elem);

    /*
     * mcan_set_filter_element() 要求配置模式（CCCR.INIT=1 且 CCE=1）。
     * 控制器运行中调用时临时进入配置模式，配置完成后恢复。
     * 注意：进入 INIT 会中止当前总线参与（未完成发送被取消），
     *       建议在启动阶段完成过滤器配置。
     */
    bool need_restore = ((base->CCCR & (MCAN_CCCR_INIT_MASK | MCAN_CCCR_CCE_MASK)) !=
                         (MCAN_CCCR_INIT_MASK | MCAN_CCCR_CCE_MASK));

    if (need_restore) {
        uint32_t retry = 0U;

        base->CCCR |= MCAN_CCCR_INIT_MASK;
        /* INIT 位跨时钟域同步，需回读确认生效 */
        while (((base->CCCR & MCAN_CCCR_INIT_MASK) == 0U) && (retry < 100000U)) {
            retry++;
        }
        if ((base->CCCR & MCAN_CCCR_INIT_MASK) == 0U) {
            return -1;
        }
        base->CCCR |= MCAN_CCCR_CCE_MASK;
    }

    hpm_stat_t status = mcan_set_filter_element(base, &sdk_elem, index);

    if (need_restore) {
        base->CCCR &= ~MCAN_CCCR_CCE_MASK;
        base->CCCR &= ~MCAN_CCCR_INIT_MASK;
    }

    return (status == status_success) ? 0 : -1;
}

/* ============================================================================
 * 接口实现 — enable_interrupt / disable_interrupt
 * ============================================================================ */

/**
 * @brief 使能事件中断
 * @param inst_id 实例号
 * @param event_mask 事件掩码
 * @return 0 = 成功；-1 = 实例无效
 */
static int mcan_enable_interrupt_impl(uint8_t inst_id, uint32_t event_mask)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || inst == NULL) {
        return -1;
    }
    uint32_t sdk_mask = mcan_event_to_sdk_mask(event_mask);
    mcan_enable_interrupts(base, sdk_mask);
    inst->interrupt_mask |= event_mask;
    return 0;
}

/**
 * @brief 禁用事件中断
 * @param inst_id 实例号
 * @param event_mask 事件掩码
 * @return 0 = 成功；-1 = 实例无效
 */
static int mcan_disable_interrupt_impl(uint8_t inst_id, uint32_t event_mask)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || inst == NULL) {
        return -1;
    }
    uint32_t sdk_mask = mcan_event_to_sdk_mask(event_mask);
    mcan_disable_interrupts(base, sdk_mask);
    inst->interrupt_mask &= ~event_mask;
    return 0;
}

/* ============================================================================
 * 接口实现 — config_irq_callback
 * ============================================================================ */

/**
 * @brief 配置中断回调（临界区内更新并挂/摘 PLIC）
 * @param inst_id 实例号
 * @param cb 回调
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 实例无效
 */
static int mcan_config_irq_callback_impl(uint8_t inst_id,
                                           intf_can_irq_callback_t cb,
                                           void *user_data)
{
    mcan_instance_t *inst = mcan_get_instance(inst_id);
    uint32_t irq_state;

    if (inst == NULL) {
        return -1;
    }

    irq_state = drv_can_enter_critical();
    inst->irq_cb = cb;
    inst->irq_user_data = user_data;
    if (inst->initialized && inst->interrupt_mask != 0U) {
        if (cb != NULL) {
            intc_m_enable_irq_with_priority(inst->irq_num, 1);
        } else {
            intc_m_disable_irq(inst->irq_num);
        }
    }
    drv_can_exit_critical(irq_state);
    return 0;
}

/* ============================================================================
 * 接口实现 — get_status (复用 SDK mcan_parse_protocol_status!)
 * ============================================================================ */

/**
 * @brief 获取协议状态与错误计数
 * @param inst_id 实例号
 * @param status 状态输出
 * @return 0 = 成功；-1 = 参数非法
 */
static int mcan_get_status_impl(uint8_t inst_id, intf_can_status_t *status)
{
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || status == NULL) {
        return -1;
    }

    mcan_protocol_status_t ps;
    mcan_get_protocol_status(base, &ps);

    mcan_error_count_t err_cnt;
    mcan_get_error_counter(base, &err_cnt);

    status->tx_error_count     = err_cnt.transmit_error_count;
    status->rx_error_count     = err_cnt.receive_error_count;
    status->last_error_code    = (uint8_t)ps.last_error_code;
    status->activity           = (uint8_t)ps.activity;
    status->bus_off            = ps.in_bus_off_state;
    status->error_warning      = ps.in_warning_state;
    status->error_passive      = ps.in_error_passive_state;
    status->protocol_exception = ps.protocol_exception_evt_occurred;
    return 0;
}

/* ============================================================================
 * 接口实现 — read_tx_event
 * ============================================================================ */

/**
 * @brief 读取发送事件 FIFO
 * @param inst_id 实例号
 * @param tx_evt 发送事件输出
 * @return 0 = 成功；-1 = 参数非法或 FIFO 空
 */
static int mcan_read_tx_event_impl(uint8_t inst_id, intf_can_tx_event_t *tx_evt)
{
    MCAN_Type *base = mcan_get_base(inst_id);
    if (base == NULL || tx_evt == NULL) {
        return -1;
    }

    mcan_tx_event_fifo_elem_t sdk_evt;
    hpm_stat_t status = mcan_read_tx_evt_fifo(base, &sdk_evt);
    if (status != status_success) {
        return -1;
    }

    tx_evt->id = sdk_evt.extend_id ? sdk_evt.ext_id : sdk_evt.std_id;
    tx_evt->is_ext_id = (sdk_evt.extend_id != 0U);
    tx_evt->is_remote = (sdk_evt.rtr != 0U);
    tx_evt->event_type = (uint8_t)sdk_evt.event_type;
    tx_evt->message_marker = sdk_evt.message_marker;
    tx_evt->timestamp = sdk_evt.tx_timestamp;
    return 0;
}

/* ============================================================================
 * 接口实现 — get_timestamp
 * ============================================================================ */

/**
 * @brief 从发送事件中提取时间戳
 * @param inst_id 实例号（未使用）
 * @param tx_evt 发送事件
 * @param ts 时间戳输出
 * @return 0 = 成功；-1 = 参数非法
 */
static int mcan_get_timestamp_impl(uint8_t inst_id,
                                    const intf_can_tx_event_t *tx_evt,
                                    intf_can_timestamp_t *ts)
{
    (void)inst_id;
    if (tx_evt == NULL || ts == NULL) {
        return -1;
    }
    ts->is_64bit = false;
    ts->ts_low = tx_evt->timestamp;
    ts->ts_high = 0U;
    return 0;
}

/* ============================================================================
 * ISR 处理 (复用 SDK demo 模式)
 * ============================================================================ */

/**
 * @brief MCAN 中断处理：映射事件并触发回调
 * @param inst_id 实例号
 */
static void mcan_isr_handler(uint8_t inst_id)
{
    mcan_instance_t *inst = &s_mcan_instances[inst_id];
    if (inst->base == NULL) {
        return;
    }

    uint32_t flags = mcan_get_interrupt_flags(inst->base);
    if (flags == 0U) {
        return;
    }

    uint32_t intf_events = sdk_mask_to_mcan_event(flags);

    mcan_clear_interrupt_flags(inst->base, flags);

    if (inst->irq_cb != NULL && intf_events != 0U) {
        inst->irq_cb(inst_id, intf_events, inst->irq_user_data);
    }
}

#if defined(HPM_MCAN0)
SDK_DECLARE_EXT_ISR_M(IRQn_MCAN0, isr_mcan0)
void isr_mcan0(void) { mcan_isr_handler(0); }
#endif

#if defined(HPM_MCAN1)
SDK_DECLARE_EXT_ISR_M(IRQn_MCAN1, isr_mcan1)
void isr_mcan1(void) { mcan_isr_handler(1); }
#endif

#if defined(HPM_MCAN2)
SDK_DECLARE_EXT_ISR_M(IRQn_MCAN2, isr_mcan2)
void isr_mcan2(void) { mcan_isr_handler(2); }
#endif

#if defined(HPM_MCAN3)
SDK_DECLARE_EXT_ISR_M(IRQn_MCAN3, isr_mcan3)
void isr_mcan3(void) { mcan_isr_handler(3); }
#endif

/* ============================================================================
 * 实例信息表初始化
 * ============================================================================ */

/**
 * @brief 初始化实例信息表（基地址/时钟/中断号，幂等）
 */
static void mcan_init_instance_table(void)
{
    static bool table_initialized = false;
    if (table_initialized) {
        return;
    }
    table_initialized = true;

#if defined(HPM_MCAN0)
    s_mcan_instances[0].base = HPM_MCAN0;
    s_mcan_instances[0].clock_name = clock_can0;
    s_mcan_instances[0].irq_num = IRQn_MCAN0;
    s_mcan_instances[0].ram_base = (uint32_t)&mcan0_msg_buf;
    s_mcan_instances[0].ram_size = sizeof(mcan0_msg_buf);
#endif

#if defined(HPM_MCAN1)
    s_mcan_instances[1].base = HPM_MCAN1;
    s_mcan_instances[1].clock_name = clock_can1;
    s_mcan_instances[1].irq_num = IRQn_MCAN1;
    s_mcan_instances[1].ram_base = (uint32_t)&mcan1_msg_buf;
    s_mcan_instances[1].ram_size = sizeof(mcan1_msg_buf);
#endif

#if defined(HPM_MCAN2)
    s_mcan_instances[2].base = HPM_MCAN2;
    s_mcan_instances[2].clock_name = clock_can2;
    s_mcan_instances[2].irq_num = IRQn_MCAN2;
    s_mcan_instances[2].ram_base = (uint32_t)&mcan2_msg_buf;
    s_mcan_instances[2].ram_size = sizeof(mcan2_msg_buf);
#endif

#if defined(HPM_MCAN3)
    s_mcan_instances[3].base = HPM_MCAN3;
    s_mcan_instances[3].clock_name = clock_can3;
    s_mcan_instances[3].irq_num = IRQn_MCAN3;
    s_mcan_instances[3].ram_base = (uint32_t)&mcan3_msg_buf;
    s_mcan_instances[3].ram_size = sizeof(mcan3_msg_buf);
#endif
}

/* ============================================================================
 * 每实例 init 包装 (捕获 inst_id)
 * ============================================================================ */

#if defined(HPM_MCAN0)
/**
 * @brief MCAN0 初始化包装
 * @param cfg CAN 配置
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_init(const intf_can_cfg_t *cfg) { return mcan_init_impl(0, cfg); }

/**
 * @brief MCAN0 反初始化包装
 */
static void mcan0_deinit(void) { mcan_deinit_impl(0); }

/**
 * @brief MCAN0 阻塞发送包装
 * @param frame CAN 帧
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_send(const intf_can_frame_t *frame, uint32_t timeout) { return mcan_send_impl(0, frame, timeout); }

/**
 * @brief MCAN0 非阻塞发送包装
 * @param frame CAN 帧
 * @param idx 发送 FIFO 索引输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_send_nb(const intf_can_frame_t *frame, uint8_t *idx) { return mcan_send_nonblocking_impl(0, frame, idx); }

/**
 * @brief MCAN0 发送请求包装
 * @param idx 发送 FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_send_add_req(uint8_t idx) { return mcan_send_add_request_impl(0, idx); }

/**
 * @brief MCAN0 阻塞接收包装
 * @param frame CAN 帧输出
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_receive(intf_can_frame_t *frame, uint32_t timeout) { return mcan_receive_impl(0, frame, timeout); }

/**
 * @brief MCAN0 非阻塞接收包装
 * @param frame CAN 帧输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_receive_nb(intf_can_frame_t *frame) { return mcan_receive_nonblocking_impl(0, frame); }

/**
 * @brief MCAN0 过滤器配置包装
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_cfg_filter(uint32_t index, const intf_can_filter_elem_t *elem) { return mcan_config_filter_impl(0, index, elem); }

/**
 * @brief MCAN0 使能中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_enable_int(uint32_t mask) { return mcan_enable_interrupt_impl(0, mask); }

/**
 * @brief MCAN0 禁用中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_disable_int(uint32_t mask) { return mcan_disable_interrupt_impl(0, mask); }

/**
 * @brief MCAN0 配置中断回调包装
 * @param cb 回调
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_cfg_irq_cb(intf_can_irq_callback_t cb, void *user_data) { return mcan_config_irq_callback_impl(0, cb, user_data); }

/**
 * @brief MCAN0 获取状态包装
 * @param status 状态输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_get_status(intf_can_status_t *status) { return mcan_get_status_impl(0, status); }

/**
 * @brief MCAN0 读取发送事件包装
 * @param tx_evt 发送事件输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_read_tx_evt(intf_can_tx_event_t *tx_evt) { return mcan_read_tx_event_impl(0, tx_evt); }

/**
 * @brief MCAN0 获取时间戳包装
 * @param tx_evt 发送事件
 * @param timestamp 时间戳输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan0_get_ts(const intf_can_tx_event_t *tx_evt, intf_can_timestamp_t *timestamp) { return mcan_get_timestamp_impl(0, tx_evt, timestamp); }

static const intf_can_t s_mcan0_ops = {
    .instance_id = 0,
    .init = mcan0_init,
    .deinit = mcan0_deinit,
    .send = mcan0_send,
    .send_nonblocking = mcan0_send_nb,
    .send_add_request = mcan0_send_add_req,
    .receive = mcan0_receive,
    .receive_nonblocking = mcan0_receive_nb,
    .config_filter = mcan0_cfg_filter,
    .enable_interrupt = mcan0_enable_int,
    .disable_interrupt = mcan0_disable_int,
    .config_irq_callback = mcan0_cfg_irq_cb,
    .get_status = mcan0_get_status,
    .read_tx_event = mcan0_read_tx_evt,
    .get_timestamp = mcan0_get_ts,
};
#endif

#if defined(HPM_MCAN1)
/**
 * @brief MCAN1 初始化包装
 * @param cfg CAN 配置
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_init(const intf_can_cfg_t *cfg) { return mcan_init_impl(1, cfg); }

/**
 * @brief MCAN1 反初始化包装
 */
static void mcan1_deinit(void) { mcan_deinit_impl(1); }

/**
 * @brief MCAN1 阻塞发送包装
 * @param frame CAN 帧
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_send(const intf_can_frame_t *frame, uint32_t timeout) { return mcan_send_impl(1, frame, timeout); }

/**
 * @brief MCAN1 非阻塞发送包装
 * @param frame CAN 帧
 * @param idx 发送 FIFO 索引输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_send_nb(const intf_can_frame_t *frame, uint8_t *idx) { return mcan_send_nonblocking_impl(1, frame, idx); }

/**
 * @brief MCAN1 发送请求包装
 * @param idx 发送 FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_send_add_req(uint8_t idx) { return mcan_send_add_request_impl(1, idx); }

/**
 * @brief MCAN1 阻塞接收包装
 * @param frame CAN 帧输出
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_receive(intf_can_frame_t *frame, uint32_t timeout) { return mcan_receive_impl(1, frame, timeout); }

/**
 * @brief MCAN1 非阻塞接收包装
 * @param frame CAN 帧输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_receive_nb(intf_can_frame_t *frame) { return mcan_receive_nonblocking_impl(1, frame); }

/**
 * @brief MCAN1 过滤器配置包装
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_cfg_filter(uint32_t index, const intf_can_filter_elem_t *elem) { return mcan_config_filter_impl(1, index, elem); }

/**
 * @brief MCAN1 使能中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_enable_int(uint32_t mask) { return mcan_enable_interrupt_impl(1, mask); }

/**
 * @brief MCAN1 禁用中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_disable_int(uint32_t mask) { return mcan_disable_interrupt_impl(1, mask); }

/**
 * @brief MCAN1 配置中断回调包装
 * @param cb 回调
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_cfg_irq_cb(intf_can_irq_callback_t cb, void *user_data) { return mcan_config_irq_callback_impl(1, cb, user_data); }

/**
 * @brief MCAN1 获取状态包装
 * @param status 状态输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_get_status(intf_can_status_t *status) { return mcan_get_status_impl(1, status); }

/**
 * @brief MCAN1 读取发送事件包装
 * @param tx_evt 发送事件输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_read_tx_evt(intf_can_tx_event_t *tx_evt) { return mcan_read_tx_event_impl(1, tx_evt); }

/**
 * @brief MCAN1 获取时间戳包装
 * @param tx_evt 发送事件
 * @param timestamp 时间戳输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan1_get_ts(const intf_can_tx_event_t *tx_evt, intf_can_timestamp_t *timestamp) { return mcan_get_timestamp_impl(1, tx_evt, timestamp); }

static const intf_can_t s_mcan1_ops = {
    .instance_id = 1,
    .init = mcan1_init,
    .deinit = mcan1_deinit,
    .send = mcan1_send,
    .send_nonblocking = mcan1_send_nb,
    .send_add_request = mcan1_send_add_req,
    .receive = mcan1_receive,
    .receive_nonblocking = mcan1_receive_nb,
    .config_filter = mcan1_cfg_filter,
    .enable_interrupt = mcan1_enable_int,
    .disable_interrupt = mcan1_disable_int,
    .config_irq_callback = mcan1_cfg_irq_cb,
    .get_status = mcan1_get_status,
    .read_tx_event = mcan1_read_tx_evt,
    .get_timestamp = mcan1_get_ts,
};
#endif

#if defined(HPM_MCAN2)
/**
 * @brief MCAN2 初始化包装
 * @param cfg CAN 配置
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_init(const intf_can_cfg_t *cfg) { return mcan_init_impl(2, cfg); }

/**
 * @brief MCAN2 反初始化包装
 */
static void mcan2_deinit(void) { mcan_deinit_impl(2); }

/**
 * @brief MCAN2 阻塞发送包装
 * @param frame CAN 帧
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_send(const intf_can_frame_t *frame, uint32_t timeout) { return mcan_send_impl(2, frame, timeout); }

/**
 * @brief MCAN2 非阻塞发送包装
 * @param frame CAN 帧
 * @param idx 发送 FIFO 索引输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_send_nb(const intf_can_frame_t *frame, uint8_t *idx) { return mcan_send_nonblocking_impl(2, frame, idx); }

/**
 * @brief MCAN2 发送请求包装
 * @param idx 发送 FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_send_add_req(uint8_t idx) { return mcan_send_add_request_impl(2, idx); }

/**
 * @brief MCAN2 阻塞接收包装
 * @param frame CAN 帧输出
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_receive(intf_can_frame_t *frame, uint32_t timeout) { return mcan_receive_impl(2, frame, timeout); }

/**
 * @brief MCAN2 非阻塞接收包装
 * @param frame CAN 帧输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_receive_nb(intf_can_frame_t *frame) { return mcan_receive_nonblocking_impl(2, frame); }

/**
 * @brief MCAN2 过滤器配置包装
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_cfg_filter(uint32_t index, const intf_can_filter_elem_t *elem) { return mcan_config_filter_impl(2, index, elem); }

/**
 * @brief MCAN2 使能中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_enable_int(uint32_t mask) { return mcan_enable_interrupt_impl(2, mask); }

/**
 * @brief MCAN2 禁用中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_disable_int(uint32_t mask) { return mcan_disable_interrupt_impl(2, mask); }

/**
 * @brief MCAN2 配置中断回调包装
 * @param cb 回调
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_cfg_irq_cb(intf_can_irq_callback_t cb, void *user_data) { return mcan_config_irq_callback_impl(2, cb, user_data); }

/**
 * @brief MCAN2 获取状态包装
 * @param status 状态输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_get_status(intf_can_status_t *status) { return mcan_get_status_impl(2, status); }

/**
 * @brief MCAN2 读取发送事件包装
 * @param tx_evt 发送事件输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_read_tx_evt(intf_can_tx_event_t *tx_evt) { return mcan_read_tx_event_impl(2, tx_evt); }

/**
 * @brief MCAN2 获取时间戳包装
 * @param tx_evt 发送事件
 * @param timestamp 时间戳输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan2_get_ts(const intf_can_tx_event_t *tx_evt, intf_can_timestamp_t *timestamp) { return mcan_get_timestamp_impl(2, tx_evt, timestamp); }

static const intf_can_t s_mcan2_ops = {
    .instance_id = 2,
    .init = mcan2_init,
    .deinit = mcan2_deinit,
    .send = mcan2_send,
    .send_nonblocking = mcan2_send_nb,
    .send_add_request = mcan2_send_add_req,
    .receive = mcan2_receive,
    .receive_nonblocking = mcan2_receive_nb,
    .config_filter = mcan2_cfg_filter,
    .enable_interrupt = mcan2_enable_int,
    .disable_interrupt = mcan2_disable_int,
    .config_irq_callback = mcan2_cfg_irq_cb,
    .get_status = mcan2_get_status,
    .read_tx_event = mcan2_read_tx_evt,
    .get_timestamp = mcan2_get_ts,
};
#endif

#if defined(HPM_MCAN3)
/**
 * @brief MCAN3 初始化包装
 * @param cfg CAN 配置
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_init(const intf_can_cfg_t *cfg) { return mcan_init_impl(3, cfg); }

/**
 * @brief MCAN3 反初始化包装
 */
static void mcan3_deinit(void) { mcan_deinit_impl(3); }

/**
 * @brief MCAN3 阻塞发送包装
 * @param frame CAN 帧
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_send(const intf_can_frame_t *frame, uint32_t timeout) { return mcan_send_impl(3, frame, timeout); }

/**
 * @brief MCAN3 非阻塞发送包装
 * @param frame CAN 帧
 * @param idx 发送 FIFO 索引输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_send_nb(const intf_can_frame_t *frame, uint8_t *idx) { return mcan_send_nonblocking_impl(3, frame, idx); }

/**
 * @brief MCAN3 发送请求包装
 * @param idx 发送 FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_send_add_req(uint8_t idx) { return mcan_send_add_request_impl(3, idx); }

/**
 * @brief MCAN3 阻塞接收包装
 * @param frame CAN 帧输出
 * @param timeout 超时毫秒语义
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_receive(intf_can_frame_t *frame, uint32_t timeout) { return mcan_receive_impl(3, frame, timeout); }

/**
 * @brief MCAN3 非阻塞接收包装
 * @param frame CAN 帧输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_receive_nb(intf_can_frame_t *frame) { return mcan_receive_nonblocking_impl(3, frame); }

/**
 * @brief MCAN3 过滤器配置包装
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_cfg_filter(uint32_t index, const intf_can_filter_elem_t *elem) { return mcan_config_filter_impl(3, index, elem); }

/**
 * @brief MCAN3 使能中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_enable_int(uint32_t mask) { return mcan_enable_interrupt_impl(3, mask); }

/**
 * @brief MCAN3 禁用中断包装
 * @param mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_disable_int(uint32_t mask) { return mcan_disable_interrupt_impl(3, mask); }

/**
 * @brief MCAN3 配置中断回调包装
 * @param cb 回调
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_cfg_irq_cb(intf_can_irq_callback_t cb, void *user_data) { return mcan_config_irq_callback_impl(3, cb, user_data); }

/**
 * @brief MCAN3 获取状态包装
 * @param status 状态输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_get_status(intf_can_status_t *status) { return mcan_get_status_impl(3, status); }

/**
 * @brief MCAN3 读取发送事件包装
 * @param tx_evt 发送事件输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_read_tx_evt(intf_can_tx_event_t *tx_evt) { return mcan_read_tx_event_impl(3, tx_evt); }

/**
 * @brief MCAN3 获取时间戳包装
 * @param tx_evt 发送事件
 * @param timestamp 时间戳输出
 * @return 0 = 成功；-1 = 失败
 */
static int mcan3_get_ts(const intf_can_tx_event_t *tx_evt, intf_can_timestamp_t *timestamp) { return mcan_get_timestamp_impl(3, tx_evt, timestamp); }

static const intf_can_t s_mcan3_ops = {
    .instance_id = 3,
    .init = mcan3_init,
    .deinit = mcan3_deinit,
    .send = mcan3_send,
    .send_nonblocking = mcan3_send_nb,
    .send_add_request = mcan3_send_add_req,
    .receive = mcan3_receive,
    .receive_nonblocking = mcan3_receive_nb,
    .config_filter = mcan3_cfg_filter,
    .enable_interrupt = mcan3_enable_int,
    .disable_interrupt = mcan3_disable_int,
    .config_irq_callback = mcan3_cfg_irq_cb,
    .get_status = mcan3_get_status,
    .read_tx_event = mcan3_read_tx_evt,
    .get_timestamp = mcan3_get_ts,
};
#endif

/* ============================================================================
 * 驱动注册 — 对 App 层暴露的唯一入口
 * ============================================================================ */

void hpm_can_driver_register(void)
{
    mcan_init_instance_table();

#if defined(HPM_MCAN0)
    intf_can_register(&s_mcan0_ops);
#endif
#if defined(HPM_MCAN1)
    intf_can_register(&s_mcan1_ops);
#endif
#if defined(HPM_MCAN2)
    intf_can_register(&s_mcan2_ops);
#endif
#if defined(HPM_MCAN3)
    intf_can_register(&s_mcan3_ops);
#endif
}

/*
 * 诊断接口：返回实例当前的外设时钟频率（Hz），0 = 实例不存在。
 * 供上层自检打印使用（不依赖 hpm_* 头文件）。
 */
uint32_t hpm_can_get_clock_freq(uint8_t inst_id)
{
    if (inst_id >= DRV_MCAN_INSTANCE_COUNT) {
        return 0U;
    }
    if (s_mcan_instances[inst_id].base == NULL) {
        return 0U;
    }
    return clock_get_frequency(s_mcan_instances[inst_id].clock_name);
}
