/**
 * @file    intf_can.h
 * @brief   CAN 抽象接口（MCAN / CAN-FD）
 * @author  Kaiser
 *
 * 通用 CAN 抽象层，覆盖 HPM MCAN 全部硬件能力。
 * 所有物理参数归一化为硬件无关类型。驱动层通过调用 HPM SDK 内置函数完成实际工作。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef INTF_CAN_H
#define INTF_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 基础类型
 * ============================================================================ */

/**
 * @brief CAN 实例编号
 */
typedef uint8_t intf_can_inst_t;

/* ============================================================================
 * CAN 工作模式 (对齐 SDK mcan_node_mode_t)
 * ============================================================================ */

/**
 * @brief CAN 工作模式
 */
typedef enum {
    INTF_CAN_MODE_NORMAL            = 0, /**< 正常收发 */
    INTF_CAN_MODE_LOOPBACK_INTERNAL = 1, /**< 内部回环（测试） */
    INTF_CAN_MODE_LOOPBACK_EXTERNAL = 2, /**< 外部回环（测试） */
    INTF_CAN_MODE_LISTEN_ONLY       = 3, /**< 只听模式（总线监控，不发 ACK） */
} intf_can_mode_t;

/* ============================================================================
 * CAN 帧类型
 * ============================================================================ */

/**
 * @brief CAN 帧类型
 */
typedef enum {
    INTF_CAN_FRAME_CLASSIC   = 0, /**< 经典 CAN 2.0 */
    INTF_CAN_FRAME_FD_NO_BRS = 1, /**< CAN FD，无比特率切换 */
    INTF_CAN_FRAME_FD_BRS    = 2, /**< CAN FD，含比特率切换 */
} intf_can_frame_type_t;

/* ============================================================================
 * 过滤器类型 (对齐 SDK mcan_filter_type_t)
 * ============================================================================ */

/**
 * @brief CAN 过滤器类型
 */
typedef enum {
    INTF_CAN_FILTER_RANGE        = 0, /**< 范围过滤：[id, mask 作为 end_id] */
    INTF_CAN_FILTER_DUAL_ID      = 1, /**< 双 ID 匹配：id + mask 作为 id2 */
    INTF_CAN_FILTER_CLASSIC      = 2, /**< ID + Mask 经典过滤 */
    INTF_CAN_FILTER_STORE_TO_BUF = 3, /**< 精确匹配 → 存入专用 RX Buffer */
} intf_can_filter_type_t;

/* ============================================================================
 * 过滤器目标 FIFO
 * ============================================================================ */

/**
 * @brief CAN 过滤器目标 FIFO
 */
typedef enum {
    INTF_CAN_FILTER_FIFO0 = 0, /**< RXFIFO0 */
    INTF_CAN_FILTER_FIFO1 = 1, /**< RXFIFO1 */
} intf_can_filter_fifo_t;

/* ============================================================================
 * 中断事件掩码 (对齐 SDK MCAN_INT_* / MCAN_EVENT_* 宏)
 * ============================================================================ */

/**
 * @brief CAN 中断事件掩码
 */
typedef enum {
    INTF_CAN_EVENT_RX_FIFO0_NEW_MSG  = (1U << 0),  /**< RXFIFO0 新消息 */
    INTF_CAN_EVENT_RX_FIFO1_NEW_MSG  = (1U << 1),  /**< RXFIFO1 新消息 */
    INTF_CAN_EVENT_RX_BUF_NEW_MSG    = (1U << 2),  /**< RX Buffer 新消息 */
    INTF_CAN_EVENT_RX_FIFO0_FULL     = (1U << 3),  /**< RXFIFO0 满 */
    INTF_CAN_EVENT_RX_FIFO1_FULL     = (1U << 4),  /**< RXFIFO1 满 */
    INTF_CAN_EVENT_RX_FIFO0_MSG_LOST = (1U << 5),  /**< RXFIFO0 消息丢失 */
    INTF_CAN_EVENT_RX_FIFO1_MSG_LOST = (1U << 6),  /**< RXFIFO1 消息丢失 */
    INTF_CAN_EVENT_TX_COMPLETED      = (1U << 7),  /**< 发送完成 */
    INTF_CAN_EVENT_TX_FIFO_EMPTY     = (1U << 8),  /**< TX FIFO 空 */
    INTF_CAN_EVENT_TX_CANCEL_DONE    = (1U << 9),  /**< 发送取消完成 */
    INTF_CAN_EVENT_TX_EVT_FIFO_NEW   = (1U << 10), /**< TX Event FIFO 新条目 */
    INTF_CAN_EVENT_TX_EVT_FIFO_FULL  = (1U << 11), /**< TX Event FIFO 满 */
    INTF_CAN_EVENT_TX_EVT_FIFO_LOST  = (1U << 12), /**< TX Event FIFO 条目丢失 */
    INTF_CAN_EVENT_BUS_OFF           = (1U << 13), /**< 总线关闭 */
    INTF_CAN_EVENT_ERROR_WARNING     = (1U << 14), /**< 错误警告 */
    INTF_CAN_EVENT_ERROR_PASSIVE     = (1U << 15), /**< 错误被动 */
    INTF_CAN_EVENT_PROTOCOL_ERROR    = (1U << 16), /**< 协议错误 */
    INTF_CAN_EVENT_TIMEOUT           = (1U << 17), /**< 超时 */
    INTF_CAN_EVENT_HIGH_PRIORITY_MSG = (1U << 18), /**< 高优先级消息 */
    INTF_CAN_EVENT_TIMESTAMP_WRAP    = (1U << 19), /**< 时间戳回绕 */
    INTF_CAN_EVENT_RAM_ACCESS_FAIL   = (1U << 20), /**< RAM 访问失败 */
} intf_can_event_t;

/* ============================================================================
 * 初始化配置 — 仅包含用户常修改的字段
 * 其余使用 SDK mcan_get_default_config() 的默认值
 * ============================================================================ */

/**
 * @brief CAN 初始化配置
 */
typedef struct {
    /* --- 必须设置 --- */
    uint32_t        baudrate;         /**< 仲裁段波特率 [bps]，如 500000 */
    intf_can_mode_t mode;             /**< 工作模式 */

    /* --- CAN FD (默认关闭) --- */
    bool            enable_canfd;     /**< 使能 CAN FD */
    uint32_t        baudrate_fd;      /**< FD 数据段波特率（enable_canfd=true 时需设置） */

    /* --- 采样点 (0=使用 SDK 默认值 75%-80%) --- */
    uint16_t        sample_point;     /**< 仲裁段采样点×10（如 800=80.0%），0=自动 */
    uint16_t        sample_point_fd;  /**< FD 数据段采样点×10，0=自动 */

    /* --- 高级节点选项 (通常false) --- */
    bool            disable_auto_retransmission; /**< 禁止自动重发 */
    bool            enable_restricted_mode;      /**< 受限操作模式（只收不发） */

    /* --- RAM 简化配置 (全部为 0 则使用 SDK 默认分配) --- */
    struct {
        uint8_t std_filter_count;  /**< 标准帧过滤器数量（0=默认） */
        uint8_t ext_filter_count;  /**< 扩展帧过滤器数量（0=默认） */
        uint8_t rxfifo0_count;     /**< RXFIFO0 元素数（0=默认） */
        uint8_t rxfifo1_count;     /**< RXFIFO1 元素数（0=默认） */
        uint8_t rxbuf_count;       /**< RX Buffer 元素数（0=默认） */
        uint8_t txbuf_count;       /**< TX Buffer/FIFO 元素数（0=默认） */
        uint8_t tx_evt_fifo_count; /**< TX Event FIFO 元素数（0=默认） */
    } ram; /**< RAM 分配配置 */

    /* --- 初始中断使能掩码 (intf_can_event_t 取或) --- */
    uint32_t        interrupt_mask;   /**< 初始中断使能掩码 */
} intf_can_cfg_t;

/* ============================================================================
 * CAN 报文帧 — 归一化类型
 * ============================================================================ */

/**
 * @brief CAN 报文帧
 */
typedef struct {
    uint32_t              id;             /**< CAN ID（标准 11 位 / 扩展 29 位） */
    bool                  is_ext_id;      /**< 是否扩展帧 */
    bool                  is_remote;      /**< 是否远程帧 */
    intf_can_frame_type_t frame_type;     /**< 帧类型 */
    uint8_t               dlc;            /**< 数据长度 0-64（已解码为实际字节数） */
    uint8_t               data[64];       /**< 数据缓冲区 */
    uint16_t              message_marker; /**< 消息标记（用于 TX Event 追踪） */
    uint32_t              timestamp;      /**< 时间戳（接收时有效） */
    uint8_t               filter_index;   /**< 匹配的过滤器索引（接收时有效） */
} intf_can_frame_t;

/* ============================================================================
 * 过滤器元素
 * ============================================================================ */

/**
 * @brief CAN 过滤器元素
 */
typedef struct {
    intf_can_filter_type_t type;        /**< 过滤类型 */
    intf_can_filter_fifo_t target_fifo; /**< 目标 FIFO（非 STORE_TO_BUF 时有效） */
    bool                   is_ext_id;   /**< 是否扩展帧过滤 */
    uint32_t               id;          /**< 过滤器 ID */
    uint32_t               mask;        /**< 屏蔽码（CLASSIC: mask; RANGE: end_id; DUAL: id2） */
    uint8_t                rxbuf_idx;   /**< STORE_TO_BUF 时的目标 Buffer 索引 */
} intf_can_filter_elem_t;

/* ============================================================================
 * 总线状态
 * ============================================================================ */

/**
 * @brief CAN 总线状态
 */
typedef struct {
    uint8_t tx_error_count;     /**< 发送错误计数 */
    uint8_t rx_error_count;     /**< 接收错误计数 */
    uint8_t last_error_code;    /**< 最后一次错误码（0=无错误） */
    uint8_t activity;           /**< 当前活动：0=同步中, 1=空闲, 2=接收, 3=发送 */
    bool    bus_off;            /**< 总线关闭 */
    bool    error_warning;      /**< 错误警告 */
    bool    error_passive;      /**< 错误被动 */
    bool    protocol_exception; /**< 协议异常事件（CAN FD） */
} intf_can_status_t;

/* ============================================================================
 * TX Event 元素
 * ============================================================================ */

/**
 * @brief CAN TX Event 元素
 */
typedef struct {
    uint32_t id;             /**< CAN ID */
    bool     is_ext_id;      /**< 是否扩展帧 */
    bool     is_remote;      /**< 是否远程帧 */
    uint8_t  event_type;     /**< 事件类型 */
    uint16_t message_marker; /**< 消息标记 */
    uint32_t timestamp;      /**< 时间戳 */
} intf_can_tx_event_t;

/* ============================================================================
 * 时间戳
 * ============================================================================ */

/**
 * @brief CAN 时间戳
 */
typedef struct {
    bool     is_64bit; /**< 是否 64 位时间戳 */
    uint32_t ts_low;   /**< 时间戳低 32 位 */
    uint32_t ts_high;  /**< 时间戳高 32 位（仅 is_64bit=true 时有效） */
} intf_can_timestamp_t;

/* ============================================================================
 * 中断回调
 * ============================================================================ */

/**
 * @brief CAN 中断回调（中断上下文执行）
 * @param inst 实例编号
 * @param event_flags 事件标志（intf_can_event_t 取或）
 * @param user_data 用户数据
 */
typedef void (*intf_can_irq_callback_t)(intf_can_inst_t inst,
                                         uint32_t event_flags,
                                         void *user_data);

/* ============================================================================
 * Interface Definition — C17 匿名结构体
 * ============================================================================ */

/**
 * @brief CAN 抽象接口
 */
typedef struct {
    uint8_t instance_id; /**< 硬件实例编号（0-3） */
    struct {
        /**
         * @brief 初始化 CAN
         * @param cfg 初始化配置
         * @return 0 = 成功；-1 = 失败
         */
        int (*init)(const intf_can_cfg_t *cfg);

        /**
         * @brief 反初始化 CAN
         */
        void (*deinit)(void);

        /**
         * @brief 发送报文（阻塞）
         * @param frame 报文
         * @param timeout_ms 超时 [ms]
         * @return 0 = 成功；-1 = 失败
         */
        int (*send)(const intf_can_frame_t *frame, uint32_t timeout_ms);

        /**
         * @brief 发送报文（非阻塞）
         * @param frame 报文
         * @param fifo_idx 输出使用的 FIFO 索引
         * @return 0 = 成功；-1 = 失败
         */
        int (*send_nonblocking)(const intf_can_frame_t *frame,
                                 uint8_t *fifo_idx);

        /**
         * @brief 请求发送已入队报文
         * @param fifo_idx FIFO 索引
         * @return 0 = 成功；-1 = 失败
         */
        int (*send_add_request)(uint8_t fifo_idx);

        /**
         * @brief 接收报文（阻塞）
         * @param frame 输出报文
         * @param timeout_ms 超时 [ms]
         * @return 0 = 成功；-1 = 失败
         */
        int (*receive)(intf_can_frame_t *frame, uint32_t timeout_ms);

        /**
         * @brief 接收报文（非阻塞）
         * @param frame 输出报文
         * @return 0 = 成功；-1 = 无数据/失败
         */
        int (*receive_nonblocking)(intf_can_frame_t *frame);

        /**
         * @brief 配置过滤器
         * @param index 过滤器索引
         * @param elem 过滤器元素
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_filter)(uint32_t index,
                              const intf_can_filter_elem_t *elem);

        /**
         * @brief 使能中断
         * @param event_mask 事件掩码
         * @return 0 = 成功；-1 = 失败
         */
        int (*enable_interrupt)(uint32_t event_mask);

        /**
         * @brief 关闭中断
         * @param event_mask 事件掩码
         * @return 0 = 成功；-1 = 失败
         */
        int (*disable_interrupt)(uint32_t event_mask);

        /**
         * @brief 配置中断回调
         * @param cb 回调（NULL = 清除）
         * @param user_data 用户数据
         * @return 0 = 成功；-1 = 失败
         */
        int (*config_irq_callback)(intf_can_irq_callback_t cb,
                                    void *user_data);

        /**
         * @brief 读取总线状态
         * @param status 输出状态
         * @return 0 = 成功；-1 = 失败
         */
        int (*get_status)(intf_can_status_t *status);

        /**
         * @brief 读取 TX Event
         * @param tx_evt 输出 TX Event
         * @return 0 = 成功；-1 = 无数据/失败
         */
        int (*read_tx_event)(intf_can_tx_event_t *tx_evt);

        /**
         * @brief 获取 TX Event 时间戳
         * @param tx_evt TX Event
         * @param ts 输出时间戳
         * @return 0 = 成功；-1 = 失败
         */
        int (*get_timestamp)(const intf_can_tx_event_t *tx_evt,
                              intf_can_timestamp_t *ts);
    };
} intf_can_t;

/* ============================================================================
 * Registration & Functional API
 * ============================================================================ */

/**
 * @brief 注册 CAN 接口实现
 * @param ops 接口实现
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_register(const intf_can_t *ops);

/**
 * @brief 初始化 CAN
 * @param inst 实例编号
 * @param cfg 初始化配置
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_init(intf_can_inst_t inst, const intf_can_cfg_t *cfg);

/**
 * @brief 反初始化 CAN
 * @param inst 实例编号
 */
void intf_can_deinit(intf_can_inst_t inst);

/**
 * @brief 发送报文（阻塞）
 * @param inst 实例编号
 * @param frame 报文
 * @param timeout_ms 超时 [ms]
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_send(intf_can_inst_t inst, const intf_can_frame_t *frame,
                    uint32_t timeout_ms);

/**
 * @brief 发送报文（非阻塞）
 * @param inst 实例编号
 * @param frame 报文
 * @param fifo_idx 输出使用的 FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_send_nonblocking(intf_can_inst_t inst,
                                const intf_can_frame_t *frame,
                                uint8_t *fifo_idx);

/**
 * @brief 请求发送已入队报文
 * @param inst 实例编号
 * @param fifo_idx FIFO 索引
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_send_add_request(intf_can_inst_t inst, uint8_t fifo_idx);

/**
 * @brief 接收报文（阻塞）
 * @param inst 实例编号
 * @param frame 输出报文
 * @param timeout_ms 超时 [ms]
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_receive(intf_can_inst_t inst, intf_can_frame_t *frame,
                       uint32_t timeout_ms);

/**
 * @brief 接收报文（非阻塞）
 * @param inst 实例编号
 * @param frame 输出报文
 * @return 0 = 成功；-1 = 无数据/失败
 */
int  intf_can_receive_nonblocking(intf_can_inst_t inst,
                                   intf_can_frame_t *frame);

/**
 * @brief 配置过滤器
 * @param inst 实例编号
 * @param index 过滤器索引
 * @param elem 过滤器元素
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_config_filter(intf_can_inst_t inst, uint32_t index,
                             const intf_can_filter_elem_t *elem);

/**
 * @brief 使能中断
 * @param inst 实例编号
 * @param event_mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_enable_interrupt(intf_can_inst_t inst, uint32_t event_mask);

/**
 * @brief 关闭中断
 * @param inst 实例编号
 * @param event_mask 事件掩码
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_disable_interrupt(intf_can_inst_t inst, uint32_t event_mask);

/**
 * @brief 配置中断回调
 * @param inst 实例编号
 * @param cb 回调（NULL = 清除）
 * @param user_data 用户数据
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_config_irq_callback(intf_can_inst_t inst,
                                   intf_can_irq_callback_t cb,
                                   void *user_data);

/**
 * @brief 读取总线状态
 * @param inst 实例编号
 * @param status 输出状态
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_get_status(intf_can_inst_t inst, intf_can_status_t *status);

/**
 * @brief 读取 TX Event
 * @param inst 实例编号
 * @param tx_evt 输出 TX Event
 * @return 0 = 成功；-1 = 无数据/失败
 */
int  intf_can_read_tx_event(intf_can_inst_t inst,
                             intf_can_tx_event_t *tx_evt);

/**
 * @brief 获取 TX Event 时间戳
 * @param inst 实例编号
 * @param tx_evt TX Event
 * @param ts 输出时间戳
 * @return 0 = 成功；-1 = 失败
 */
int  intf_can_get_timestamp(intf_can_inst_t inst,
                             const intf_can_tx_event_t *tx_evt,
                             intf_can_timestamp_t *ts);

#ifdef __cplusplus
}
#endif

#endif /* INTF_CAN_H */
