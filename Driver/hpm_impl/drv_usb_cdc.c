/**
 * @file    drv_usb_cdc.c
 * @brief   USB 复合设备驱动 - HPM USB0（CherryUSB 设备栈：CDC ACM + WinUSB bulk）
 * @author  Kaiser
 *
 * 实现说明：
 *   - 复用 SDK 中间件 CherryUSB（middleware/cherryusb）：设备栈 + CDC ACM 类 + HPM 端口
 *   - 接口 0：厂商自定义（WinUSB，bulk IN/OUT）—— 二进制高速通道
 *   - 接口 1/2：CDC ACM —— Terminal 文本通道（行为与改动前一致）
 *   - MS OS 2.0 描述符（BOS + WCID）使 Windows 10/11 免驱绑定 WinUSB，无需 Zadig
 *   - RX：OUT 端点中断回调 → SPSC 环形缓冲（+ 可选用户回调，中断上下文执行）
 *   - TX：拷贝至非缓存缓冲 → usbd_ep_start_write → 等待完成（mcycle 超时）
 *   - 板级：USB0 时钟与 PHY（内部 VBUS）由 board_init_usb() 完成；
 *     HPM53M1 的 USB_DP/USB_DM 为专用引脚（pin48/49），无 IOMUX 配置
 *   - DTR：上位机打开虚拟串口后置位，可经 intf_usb_cdc_t::is_dtr() 查询
 *
 * P0 阶段：bulk 通道仅做「回环自检」（OUT 收到什么就原样发回 IN），用于验证
 *   复合设备枚举与端点通路；P1 将由二进制协议服务替换。
 *
 * 约束：单次 write ≤ USB_CDC_TX_BUF_SIZE（512B）
 * timeout_ms 语义（write）：0 = 不等待、UINT32_MAX = 无限、其他 = 毫秒
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_usb_cdc.h"
#include "intf_usb_bulk.h"
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

/* 厂商自定义（WinUSB）接口：interface 0，位于 CDC 之前 */
#define USB_BULK_IN_EP       (0x82U)
#define USB_BULK_OUT_EP      (0x02U)
#define USB_BULK_MPS_HS      (512U)
#define USB_BULK_MPS_FS      (64U)

#define USB_CDC_EP_MPS       (512U)
#define USB_CDC_TX_BUF_SIZE  (512U)
#define USB_CDC_RX_RING_SIZE (512U) /* 必须为 2 的幂 */
#define USB_CDC_RX_RING_MASK (USB_CDC_RX_RING_SIZE - 1U)

/* ============================================================================
 * 描述符（参考 SDK 样例 cherryusb/demo/winusb2.0_cdc_template.c）
 * ============================================================================ */

/* interface 0 描述符 + 2 个 bulk 端点 */
#define USB_VENDOR_INTF_SIZE (9 + 7 + 7)

#define USB_CONFIG_SIZE     (9 + USB_VENDOR_INTF_SIZE + CDC_ACM_DESCRIPTOR_LEN)
#define USB_CONFIG_INTF_NUM (3U) /* vendor + CDC 控制 + CDC 数据 */

/* MS OS 2.0：Windows 据 WCID 自动为 interface 0 绑定 WinUSB（Win10/11 免驱） */
#define USB_WINUSB_VENDOR_CODE (0x17U)

__ALIGN_BEGIN static const uint8_t s_winusb_wcid_descriptor[] = {
    USB_MSOSV2_COMP_ID_SET_HEADER_DESCRIPTOR_INIT(
        10U + USB_MSOSV2_COMP_ID_FUNCTION_WINUSB_MULTI_DESCRIPTOR_LEN),
    USB_MSOSV2_COMP_ID_FUNCTION_WINUSB_MULTI_DESCRIPTOR_INIT(0x00U),
};

__ALIGN_BEGIN static const uint8_t s_bos_descriptor_data[] = {
    USB_BOS_HEADER_DESCRIPTOR_INIT(5U + USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_LEN, 1U),
    USB_BOS_CAP_PLATFORM_WINUSB_DESCRIPTOR_INIT(USB_WINUSB_VENDOR_CODE,
                                                sizeof(s_winusb_wcid_descriptor)),
};

static const struct usb_msosv2_descriptor s_msosv2_descriptor = {
    .vendor_code = USB_WINUSB_VENDOR_CODE,
    .compat_id = s_winusb_wcid_descriptor,
    .compat_id_len = sizeof(s_winusb_wcid_descriptor),
};

static const struct usb_bos_descriptor s_bos_descriptor = {
    .string = s_bos_descriptor_data,
    .string_len = sizeof(s_bos_descriptor_data),
};

static const uint8_t s_device_descriptor[] = {
    /* bcdUSB 2.1：BOS 描述符要求 ≥ 0x0210 */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t s_config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USB_CONFIG_INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED,
                               USBD_MAX_POWER),
    /* Interface 0：vendor specific（WinUSB） */
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_OUT_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_HS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_IN_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_HS, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x02),
};

static const uint8_t s_config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USB_CONFIG_INTF_NUM, 0x01, USB_CONFIG_BUS_POWERED,
                               USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_OUT_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_FS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_IN_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_FS, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x02),
};

static const uint8_t s_device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, 0x01),
};

static const uint8_t s_other_speed_config_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USB_CONFIG_INTF_NUM, 0x01,
                                           USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_OUT_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_FS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_IN_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_FS, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_FS, 0x02),
};

static const uint8_t s_other_speed_config_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, USB_CONFIG_INTF_NUM, 0x01,
                                           USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_OUT_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_HS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BULK_IN_EP, USB_ENDPOINT_TYPE_BULK, USB_BULK_MPS_HS, 0x00),
    CDC_ACM_DESCRIPTOR_INIT(0x01, USB_CDC_INT_EP, USB_CDC_OUT_EP, USB_CDC_IN_EP, USB_BULK_EP_MPS_HS, 0x02),
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

static const struct usb_descriptor s_usb_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .other_speed_descriptor_callback = other_speed_config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .msosv2_descriptor = &s_msosv2_descriptor,
    .bos_descriptor = &s_bos_descriptor,
};

/* ============================================================================
 * 状态与缓冲
 * ============================================================================ */

static volatile bool s_configured;
static volatile bool s_dtr;
static volatile bool s_tx_busy;
static uint32_t s_tx_start_cycle; /**< 本次发送起始 cycle（卡死恢复判定） */

/* 运行统计（链路稳定性诊断；Ozone 可直读） */
static volatile uint32_t s_stat_tx_timeouts;
static volatile uint32_t s_stat_tx_drops;
static volatile uint32_t s_stat_rx_drops;
static volatile uint32_t s_stat_bus_resets;
static volatile uint32_t s_stat_xfer_errors;

/* TX 完成等待上限 [ms]：控制器传输出错时 SDK 不回调完成 → 超时强制恢复，
 * 否则 s_tx_busy 永久为真、TX 死锁（现象：终端静默，只能重新枚举恢复）。 */
#define USB_CDC_TX_STALL_TIMEOUT_MS (50U)
static bool s_initialized;
static intf_usb_cdc_rx_cb_t s_rx_cb;

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_ep_out_buf[USB_CDC_EP_MPS];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_ep_in_buf[USB_CDC_TX_BUF_SIZE];

/* ---- 厂商自定义接口（WinUSB bulk，二进制协议通道） ---- */
#define USB_BULK_RX_RING_SIZE (1024U) /* 必须为 2 的幂 */
#define USB_BULK_RX_RING_MASK (USB_BULK_RX_RING_SIZE - 1U)
#define USB_BULK_TX_BUF_SIZE  (1024U) /* ≥ 单帧上限 9 + MP_MAX_PAYLOAD */

static volatile bool s_bulk_tx_busy;
static uint32_t s_bulk_tx_start_cycle; /**< 本次发送起始 cycle（卡死恢复判定） */

static volatile uint32_t s_stat_bulk_rx_packets; /**< 收到的 OUT 包数 */
static volatile uint32_t s_stat_bulk_tx_packets; /**< 成功排队的 IN 包数 */
static volatile uint32_t s_stat_bulk_rx_drops;   /**< 接收环满丢弃的字节数 */
static volatile uint32_t s_stat_bulk_tx_drops;   /**< IN 忙/失败丢弃的字节数 */
static volatile uint32_t s_stat_bulk_oversize;   /**< 超 MPS 的异常包数 */

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_bulk_out_buf[USB_BULK_MPS_HS];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t s_bulk_tx_buf[USB_BULK_TX_BUF_SIZE];

/* SPSC 环形缓冲：head 仅 USB ISR 写，tail 仅任务写（RV32 上 16bit 访问原子） */
static uint8_t s_bulk_rx_ring[USB_BULK_RX_RING_SIZE];
static volatile uint16_t s_bulk_rx_head;
static volatile uint16_t s_bulk_rx_tail;

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
        s_stat_rx_drops++; /* 满：丢弃新字节 */
        return;
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
        s_bulk_tx_busy = false;
        s_bulk_rx_head = 0U;
        s_bulk_rx_tail = 0U;
        s_stat_bus_resets++;
        break;
    case USBD_EVENT_ERROR:
        s_stat_xfer_errors++;
        /* 控制器报错：完成回调不会到达，且 OUT 端点可能已停止接收 → 重新武装 */
        if (s_configured) {
            usbd_ep_start_read(busid, USB_CDC_OUT_EP, s_ep_out_buf,
                               usbd_get_ep_mps(busid, USB_CDC_OUT_EP));
            usbd_ep_start_read(busid, USB_BULK_OUT_EP, s_bulk_out_buf,
                               usbd_get_ep_mps(busid, USB_BULK_OUT_EP));
        }
        break;
    case USBD_EVENT_CONFIGURED:
        s_configured = true;
        usbd_ep_start_read(busid, USB_CDC_OUT_EP, s_ep_out_buf,
                           usbd_get_ep_mps(busid, USB_CDC_OUT_EP));
        usbd_ep_start_read(busid, USB_BULK_OUT_EP, s_bulk_out_buf,
                           usbd_get_ep_mps(busid, USB_BULK_OUT_EP));
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

/* ---- 厂商自定义接口（WinUSB bulk，二进制协议通道） ---- */

/**
 * @brief 向 bulk 接收环压入数据（USB 中断上下文；满则丢弃多余字节）
 * @param data 数据
 * @param len 长度 [byte]
 */
static void usb_bulk_ring_push(const uint8_t *data, uint32_t len)
{
    uint16_t head = s_bulk_rx_head;
    uint16_t tail = s_bulk_rx_tail;
    uint16_t used = (head >= tail) ? (uint16_t) (head - tail)
                                   : (uint16_t) (USB_BULK_RX_RING_SIZE - tail + head);
    uint32_t count = (uint32_t) (USB_BULK_RX_RING_SIZE - 1U - used);
    uint32_t first;

    if (count > len) {
        count = len;
    }
    if (count < len) {
        s_stat_bulk_rx_drops += (len - count);
    }

    first = (uint32_t) (USB_BULK_RX_RING_SIZE - head);
    if (first > count) {
        first = count;
    }
    memcpy(&s_bulk_rx_ring[head], data, first);
    if (count > first) {
        memcpy(&s_bulk_rx_ring[0], &data[first], count - first);
    }
    s_bulk_rx_head = (uint16_t) ((head + count) & USB_BULK_RX_RING_MASK);
}

/**
 * @brief bulk OUT 完成：数据压入接收环后立即重臂
 * @param busid 总线 ID
 * @param ep 端点地址
 * @param nbytes 收到的字节数
 */
static void usbd_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    s_stat_bulk_rx_packets++;

    if (nbytes > sizeof(s_bulk_out_buf)) {
        s_stat_bulk_oversize++;
    } else if (nbytes > 0U) {
        usb_bulk_ring_push(s_bulk_out_buf, nbytes);
    }

    usbd_ep_start_read(busid, ep, s_bulk_out_buf, usbd_get_ep_mps(busid, ep));
}

/**
 * @brief bulk IN 完成：MPS 整数倍时补 ZLP
 * @param busid 总线 ID
 * @param ep 端点地址
 * @param nbytes 已发送字节数
 */
static void usbd_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (((nbytes % usbd_get_ep_mps(busid, ep)) == 0U) && (nbytes != 0U)) {
        usbd_ep_start_write(busid, ep, NULL, 0);
    } else {
        s_bulk_tx_busy = false;
    }
}

static struct usbd_endpoint s_bulk_out_ep = {
    .ep_addr = USB_BULK_OUT_EP,
    .ep_cb = usbd_bulk_out,
};

static struct usbd_endpoint s_bulk_in_ep = {
    .ep_addr = USB_BULK_IN_EP,
    .ep_cb = usbd_bulk_in,
};

/* 厂商接口无类请求处理器：仅标准请求由核心处理 */
static struct usbd_interface s_vendor_intf;

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
    s_bulk_tx_busy = false;
    s_bulk_rx_head = 0U;
    s_bulk_rx_tail = 0U;
    s_configured = false;
    s_dtr = false;
    s_rx_cb = NULL;

    /* USB 低于 ADC/控制类中断（ADC0/FOC=3、GPTMR1=2），避免枚举和
     * CDC ISR 抢占控制链路。 */
    intc_set_irq_priority(IRQn_USB0, 1);

    /* 接口编号由 usbd_add_interface 顺序分配，必须与配置描述符顺序一致：
     * 0 = vendor(WinUSB)，1 = CDC 控制，2 = CDC 数据 */
    usbd_desc_register(USB_CDC_BUSID, &s_usb_descriptor);
    usbd_add_interface(USB_CDC_BUSID, &s_vendor_intf);
    usbd_add_interface(USB_CDC_BUSID, usbd_cdc_acm_init_intf(USB_CDC_BUSID, &s_cdc_intf0));
    usbd_add_interface(USB_CDC_BUSID, usbd_cdc_acm_init_intf(USB_CDC_BUSID, &s_cdc_intf1));
    usbd_add_endpoint(USB_CDC_BUSID, &s_bulk_out_ep);
    usbd_add_endpoint(USB_CDC_BUSID, &s_bulk_in_ep);
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

    /* 上一次发送卡死（控制器报错/主机长时间不取）：超时强制恢复，避免 TX 永久死锁 */
    if (s_tx_busy
        && ((uint32_t)(intf_clock_get_cycle() - s_tx_start_cycle)
            >= usb_cdc_ms_to_cycles(USB_CDC_TX_STALL_TIMEOUT_MS))) {
        s_stat_tx_timeouts++;
        s_tx_busy = false;
        (void)usbd_ep_clear_stall(USB_CDC_BUSID, USB_CDC_IN_EP);
    }

    /* 等待上一次发送完成 */
    while (s_tx_busy) {
        if (usb_cdc_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            s_stat_tx_drops++;
            return -1;
        }
    }

    memcpy(s_ep_in_buf, data, len);
    s_tx_start_cycle = intf_clock_get_cycle();
    s_tx_busy = true;
    if (usbd_ep_start_write(USB_CDC_BUSID, USB_CDC_IN_EP, s_ep_in_buf, (uint32_t) len) != 0) {
        s_tx_busy = false; /* 启动失败：不留下永久 busy */
        s_stat_tx_drops++;
        return -1;
    }

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
static void hpm_usb_cdc_get_stats(intf_usb_cdc_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->tx_timeouts = s_stat_tx_timeouts;
    out->tx_drops = s_stat_tx_drops;
    out->rx_drops = s_stat_rx_drops;
    out->bus_resets = s_stat_bus_resets;
    out->xfer_errors = s_stat_xfer_errors;
}

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
 * USB bulk 接口实现（与 CDC 共用同一台设备）
 * ============================================================================ */

/**
 * @brief 发送一帧到 bulk IN（任务上下文；阻塞至完成或超时）
 * @param data 发送缓冲
 * @param len 长度 [byte]
 * @param timeout_ms 超时毫秒语义
 * @return 0 = 成功；-1 = 参数非法、未就绪或超时
 */
static int hpm_usb_bulk_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    uint32_t start;
    uint32_t timeout_cycles;

    if ((data == NULL) || (len == 0U) || (len > USB_BULK_TX_BUF_SIZE)) {
        return -1;
    }
    if (!s_initialized || !s_configured) {
        return -1;
    }

    start = intf_clock_get_cycle();
    timeout_cycles = usb_cdc_ms_to_cycles(timeout_ms);

    /* 上一次发送卡死（控制器报错/主机长时间不取）：超时强制恢复，避免 TX 永久死锁 */
    if (s_bulk_tx_busy
        && ((uint32_t) (intf_clock_get_cycle() - s_bulk_tx_start_cycle)
            >= usb_cdc_ms_to_cycles(USB_CDC_TX_STALL_TIMEOUT_MS))) {
        s_bulk_tx_busy = false;
        (void) usbd_ep_clear_stall(USB_CDC_BUSID, USB_BULK_IN_EP);
    }

    while (s_bulk_tx_busy) {
        if (usb_cdc_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            s_stat_bulk_tx_drops += (uint32_t) len;
            return -1;
        }
    }

    memcpy(s_bulk_tx_buf, data, len);
    s_bulk_tx_start_cycle = intf_clock_get_cycle();
    s_bulk_tx_busy = true;
    if (usbd_ep_start_write(USB_CDC_BUSID, USB_BULK_IN_EP, s_bulk_tx_buf, (uint32_t) len) != 0) {
        s_bulk_tx_busy = false;
        s_stat_bulk_tx_drops += (uint32_t) len;
        return -1;
    }
    s_stat_bulk_tx_packets++;

    if (timeout_ms == 0U) {
        return 0;
    }
    while (s_bulk_tx_busy) {
        if (usb_cdc_timeout_elapsed(start, timeout_cycles, timeout_ms)) {
            s_stat_bulk_tx_drops += (uint32_t) len;
            return -1;
        }
    }
    return 0;
}

/**
 * @brief 从接收环读取数据（非阻塞）
 * @param data 接收缓冲
 * @param len 期望长度 [byte]
 * @return 实际接收字节数；-1 = 参数非法
 */
static int hpm_usb_bulk_read(uint8_t *data, size_t len)
{
    size_t count = 0U;

    if ((data == NULL) || (len == 0U)) {
        return -1;
    }
    while ((count < len) && (s_bulk_rx_head != s_bulk_rx_tail)) {
        data[count] = s_bulk_rx_ring[s_bulk_rx_tail];
        s_bulk_rx_tail = (uint16_t) ((s_bulk_rx_tail + 1U) & USB_BULK_RX_RING_MASK);
        count++;
    }
    return (int) count;
}

/**
 * @brief bulk 通道是否可收发
 * @return true = 已枚举完成
 */
static bool hpm_usb_bulk_is_ready(void)
{
    return s_initialized && s_configured;
}

/**
 * @brief 读取 bulk 运行统计
 * @param out 统计输出
 */
static void hpm_usb_bulk_get_stats(intf_usb_bulk_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->rx_packets = s_stat_bulk_rx_packets;
    out->tx_packets = s_stat_bulk_tx_packets;
    out->rx_drops = s_stat_bulk_rx_drops;
    out->tx_drops = s_stat_bulk_tx_drops;
    out->oversize = s_stat_bulk_oversize;
    out->bus_resets = s_stat_bus_resets;
}

static const intf_usb_bulk_t s_usb_bulk_dev = {
    .instance_id = 0U,
    .init = hpm_usb_cdc_init, /* 与 CDC 共用设备初始化（幂等） */
    .write = hpm_usb_bulk_write,
    .read = hpm_usb_bulk_read,
    .is_ready = hpm_usb_bulk_is_ready,
    .get_stats = hpm_usb_bulk_get_stats,
};

void hpm_usb_bulk_driver_register(void)
{
    (void) intf_usb_bulk_register(&s_usb_bulk_dev);
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
    .get_stats = hpm_usb_cdc_get_stats,
    .deinit = hpm_usb_cdc_deinit,
};

void hpm_usb_cdc_driver_register(void)
{
    intf_usb_cdc_register(&s_usb_cdc_dev);
}
