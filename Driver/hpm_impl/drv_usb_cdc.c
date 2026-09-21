/**
 * @file    drv_usb_cdc.c
 * @brief   USB CDC ACM 驱动 - HPM USB0 虚拟串口（CherryUSB 设备栈）
 * @author  Kaiser
 *
 * 实现说明：
 *   - 复用 SDK 中间件 CherryUSB（middleware/cherryusb）：设备栈 + CDC ACM 类 + HPM 端口
 *   - RX：OUT 端点中断回调 → SPSC 环形缓冲（+ 可选用户回调，中断上下文执行）
 *   - TX：拷贝至非缓存缓冲 → usbd_ep_start_write → 等待完成（mcycle 超时）
 *   - 板级：USB0 时钟与 PHY（内部 VBUS）由 board_init_usb() 完成；
 *     HPM53M1 的 USB_DP/USB_DM 为专用引脚（pin48/49），无 IOMUX 配置
 *   - DTR：上位机打开虚拟串口后置位，可经 intf_usb_cdc_t::is_dtr() 查询
 *
 * 约束：单次 write ≤ USB_CDC_TX_BUF_SIZE（512B）
 * timeout_ms 语义（write）：0 = 不等待、UINT32_MAX = 无限、其他 = 毫秒
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_usb_cdc.h"
#include "intf_clock.h"
#include "board.h"

#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#include "hpm_interrupt.h"
#include "hpm_soc_irq.h"

#include <string.h>

#define USB_CDC_BUSID        (0U)
#define USB_CDC_IN_EP        (0x81U)
#define USB_CDC_OUT_EP       (0x01U)
#define USB_CDC_INT_EP       (0x83U)

#define USB_CDC_EP_MPS       (512U)
#define USB_CDC_TX_BUF_SIZE  (512U)
#define USB_CDC_RX_RING_SIZE (512U) /* 必须为 2 的幂 */
#define USB_CDC_RX_RING_MASK (USB_CDC_RX_RING_SIZE - 1U)

/* ============================================================================
 * 描述符（参考 SDK 样例 cdc_acm_vcom）
 * ============================================================================ */

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t s_config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x02),
};

static const uint8_t s_config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x02),
};

static const uint8_t s_device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, 0x01),
};

static const uint8_t s_other_speed_config_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x02),
};

static const uint8_t s_other_speed_config_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x02),
};

static const char *s_string_descriptors[] = {
    (const char[]){ 0x09, 0x04 }, /* Langid */
    "HPMicro",                    /* Manufacturer */
    "HPM53M1_G6618Motor",         /* Product（与工程同名） */
    "0002",                       /* Serial Number（变更以刷新 Windows 设备名缓存） */
};

/**
 * @brief 获取设备描述符
 * @param speed USB 速度（忽略）
 * @return 设备描述符
 */
static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void) speed;
    return s_device_descriptor;
}

/**
 * @brief 按速度获取配置描述符
 * @param speed USB 速度
 * @return 高速/全速配置描述符；其他速度返回 NULL
 */
static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    if (speed == USB_SPEED_HIGH) {
        return s_config_descriptor_hs;
    } else if (speed == USB_SPEED_FULL) {
        return s_config_descriptor_fs;
    }
    return NULL;
}

/**
 * @brief 获取设备限定描述符
 * @param speed USB 速度（忽略）
 * @return 设备限定描述符
 */
static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    (void) speed;
    return s_device_quality_descriptor;
}

/**
 * @brief 按速度获取其他速度配置描述符
 * @param speed USB 速度
 * @return 高速/全速其他速度配置描述符；其他速度返回 NULL
 */
static const uint8_t *other_speed_config_descriptor_callback(uint8_t speed)
{
    if (speed == USB_SPEED_HIGH) {
        return s_other_speed_config_descriptor_hs;
    } else if (speed == USB_SPEED_FULL) {
        return s_other_speed_config_descriptor_fs;
    }
    return NULL;
}

/**
 * @brief 按索引获取字符串描述符
 * @param speed USB 速度（忽略）
 * @param index 字符串索引
 * @return 字符串描述符；越界返回 NULL
 */
static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    (void) speed;

    if (index >= (sizeof(s_string_descriptors) / sizeof(char *))) {
        return NULL;
    }
    return s_string_descriptors[index];
}

static const struct usb_descriptor s_cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
};

/* ============================================================================
 * 状态与缓冲
 * ============================================================================ */

static volatile bool s_configured;
static volatile bool s_dtr;
static volatile bool s_tx_busy;
static bool s_initialized;
static intf_usb_cdc_rx_cb_t s_rx_cb;

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_ep_out_buf[USB_CDC_EP_MPS];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_ep_in_buf[USB_CDC_TX_BUF_SIZE];

/* SPSC 环形缓冲：head 仅 ISR 写，tail 仅主循环写（RV32 上 16bit 访问原子） */
static uint8_t s_rx_ring[USB_CDC_RX_RING_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

/**
 * @brief 向 SPSC 环形缓冲压入一个字节（满则丢弃）
 * @param byte 待压入字节
 */
static inline void usb_cdc_ring_push(uint8_t byte)
{
    uint16_t next = (uint16_t) ((s_rx_head + 1U) & USB_CDC_RX_RING_MASK);

    if (next == s_rx_tail) {
        return; /* 满：丢弃新字节 */
    }
    s_rx_ring[s_rx_head & USB_CDC_RX_RING_MASK] = byte;
    s_rx_head = next;
}

/**
 * @brief 从 SPSC 环形缓冲弹出一个字节
 * @param byte 输出字节
 * @return true = 取到数据
 */
static inline bool usb_cdc_ring_pop(uint8_t *byte)
{
    if (s_rx_head == s_rx_tail) {
        return false;
    }
    *byte = s_rx_ring[s_rx_tail & USB_CDC_RX_RING_MASK];
    s_rx_tail = (uint16_t) ((s_rx_tail + 1U) & USB_CDC_RX_RING_MASK);
    return true;
}

/* timeout_ms 语义：0 = 不等待；UINT32_MAX = 无限；其他 = 毫秒 */
/**
 * @brief 毫秒转 CPU cycle
 * @param ms 毫秒数
 * @return 对应 cycle 数
 */
static uint32_t usb_cdc_ms_to_cycles(uint32_t ms)
{
    return (uint32_t) ((uint64_t) ms * (intf_clock_get_cpu_freq() / 1000U));
}

/**
 * @brief 判断超时是否到达
 * @param start 起始 cycle
 * @param timeout_cycles 超时 cycle
 * @param timeout_ms 超时毫秒语义
 * @return true = 已超时
 */
static bool usb_cdc_timeout_elapsed(uint32_t start, uint32_t timeout_cycles, uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return true;
    }
    if (timeout_ms == UINT32_MAX) {
        return false;
    }
    return (uint32_t) (intf_clock_get_cycle() - start) >= timeout_cycles;
}

/* ============================================================================
 * USB 回调（中断上下文）
 * ============================================================================ */

/**
 * @brief USB 设备事件处理（中断上下文）
 * @param busid 总线 ID
 * @param event 事件码
 */
static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
    case USBD_EVENT_RESET:
    case USBD_EVENT_DISCONNECTED:
        s_configured = false;
        s_dtr = false;
        s_tx_busy = false;
        break;
    case USBD_EVENT_CONFIGURED:
        s_configured = true;
        usbd_ep_start_read(busid, USB_CDC_OUT_EP, s_ep_out_buf,
                           usbd_get_ep_mps(busid, USB_CDC_OUT_EP));
        break;
    default:
        break;
    }
}

/* OUT 端点：接收数据 → 环形缓冲 + 用户回调 */
void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    for (uint32_t i = 0U; i < nbytes; i++) {
        usb_cdc_ring_push(s_ep_out_buf[i]);
    }
    if ((nbytes > 0U) && (s_rx_cb != NULL)) {
        s_rx_cb(s_ep_out_buf, nbytes); /* 中断上下文，data 仅在回调期间有效 */
    }
    usbd_ep_start_read(busid, ep, s_ep_out_buf, usbd_get_ep_mps(busid, ep));
}

/* IN 端点：发送完成（MPS 整数倍时补 ZLP） */
void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (((nbytes % usbd_get_ep_mps(busid, ep)) == 0U) && (nbytes != 0U)) {
        usbd_ep_start_write(busid, ep, NULL, 0);
    } else {
        s_tx_busy = false;
    }
}

/* CDC 类弱符号覆盖：上位机 DTR 状态 */
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void) busid;
    (void) intf;
    s_dtr = dtr;
}

static struct usbd_endpoint s_cdc_out_ep = {
    .ep_addr = USB_CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out,
};

static struct usbd_endpoint s_cdc_in_ep = {
    .ep_addr = USB_CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in,
};

static struct usbd_interface s_cdc_intf0;
static struct usbd_interface s_cdc_intf1;

/* ============================================================================
 * 接口实现
 * ============================================================================ */

/**
 * @brief 初始化 USB CDC 设备栈（幂等）
 * @return 0 = 成功
 */
static int hpm_usb_cdc_init(void)
{
    if (s_initialized) {
        return 0;
    }

    /* 板级：USB0 时钟 + PHY（内部 VBUS；DP/DM 下拉已在 board_init 关闭） */
    board_init_usb();

    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_tx_busy = false;
    s_configured = false;
    s_dtr = false;
    s_rx_cb = NULL;

    intc_set_irq_priority(IRQn_USB0, 2);

    usbd_desc_register(USB_CDC_BUSID, &s_cdc_descriptor);
    usbd_add_interface(USB_CDC_BUSID, usbd_cdc_acm_init_intf(USB_CDC_BUSID, &s_cdc_intf0));
    usbd_add_interface(USB_CDC_BUSID, usbd_cdc_acm_init_intf(USB_CDC_BUSID, &s_cdc_intf1));
    usbd_add_endpoint(USB_CDC_BUSID, &s_cdc_out_ep);
    usbd_add_endpoint(USB_CDC_BUSID, &s_cdc_in_ep);
    usbd_initialize(USB_CDC_BUSID, CONFIG_HPM_USBD_BASE, usbd_event_handler);

    s_initialized = true;
    return 0;
}

/**
 * @brief 发送数据（单次 ≤ USB_CDC_TX_BUF_SIZE）
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法、未就绪或超时
 */
static int hpm_usb_cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint32_t start;
    uint32_t timeout_cycles;

    if ((data == NULL) || (len == 0U) || (len > USB_CDC_TX_BUF_SIZE)) {
        return -1;
    }
    if (!s_initialized || !s_configured) {
        return -1;
    }

    start = intf_clock_get_cycle();
    timeout_cycles = usb_cdc_ms_to_cycles(timeout_ms);

    /* 等待上一次发送完成 */
    while (s_tx_busy) {
        if (usb_cdc_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            return -1;
        }
    }

    memcpy(s_ep_in_buf, data, len);
    s_tx_busy = true;
    usbd_ep_start_write(USB_CDC_BUSID, USB_CDC_IN_EP, s_ep_in_buf, (uint32_t) len);

    if (timeout_ms == 0U) {
        return 0; /* 不等待完成 */
    }

    while (s_tx_busy) {
        if (usb_cdc_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief 从环形缓冲读取数据
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @return 实际接收字节数；-1 = 参数非法
 */
static int hpm_usb_cdc_read(uint8_t *data, size_t len)
{
    size_t count = 0U;

    if ((data == NULL) || (len == 0U)) {
        return -1;
    }

    while ((count < len) && usb_cdc_ring_pop(&data[count])) {
        count++;
    }
    return (int) count;
}

/**
 * @brief 注册接收回调
 * @param cb 回调（中断上下文执行）
 * @return 0 = 成功
 */
static int hpm_usb_cdc_register_rx_callback(intf_usb_cdc_rx_cb_t cb)
{
    s_rx_cb = cb;
    return 0;
}

/**
 * @brief 查询 DTR 状态（上位机是否打开串口）
 * @return true = DTR 置位
 */
static bool hpm_usb_cdc_is_dtr(void)
{
    return s_dtr;
}

/**
 * @brief 反初始化 USB CDC 设备栈
 */
static void hpm_usb_cdc_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    usbd_deinitialize(USB_CDC_BUSID);
    s_initialized = false;
    s_configured = false;
    s_dtr = false;
    s_tx_busy = false;
    s_rx_cb = NULL;
}

/* ============================================================================
 * 单实例设备对象（风格 A）
 * ============================================================================ */

static const intf_usb_cdc_t s_usb_cdc_dev = {
    .instance_id = 0U,
    .init = hpm_usb_cdc_init,
    .write = hpm_usb_cdc_write,
    .read = hpm_usb_cdc_read,
    .register_rx_callback = hpm_usb_cdc_register_rx_callback,
    .is_dtr = hpm_usb_cdc_is_dtr,
    .deinit = hpm_usb_cdc_deinit,
};

void hpm_usb_cdc_driver_register(void)
{
    intf_usb_cdc_register(&s_usb_cdc_dev);
}
