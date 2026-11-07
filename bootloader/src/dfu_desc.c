/**
 * @file    dfu_desc.c
 * @brief   Bootloader USB 描述符：DFU 1.1 + MS OS 2.0（WCID 免驱）
 * @author  Kaiser
 *
 * 设备身份（与应用的区分只靠 PID）：
 *   VID 0x34B7 / PID 0x6619 / Serial = 芯片 OTP UUID（32 hex，逐板唯一）
 *   Manufacturer "Alliance HardwareGroup"，Product "G6618 DFU"
 *
 * WCID：BOS 内含 MS OS 2.0 Platform Capability，Windows 10 1709+ 自动绑定
 * WinUSB，无需 Zadig；dfu-util（libusb）直接可用。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"

#include "board.h"
#include "dfu.h"
#include "hpm_otp_drv.h"
#include "hpm_soc.h"

#include <string.h>

#define BOOT_USB_VID (0x34B7U)
#define BOOT_USB_PID (0x6619U)

#define MSOS20_VENDOR_CODE (0x17U)
#define MSOS20_SET_HEADER_LEN (10U)
#define MSOS20_FUNCTION_SUBSET_LEN (8U)
#define MSOS20_COMPAT_ID_LEN (20U)
#define MSOS20_REG_PROP_LEN (132U)
#define MSOS20_FUNCTION_TOTAL \
    (MSOS20_FUNCTION_SUBSET_LEN + MSOS20_COMPAT_ID_LEN + MSOS20_REG_PROP_LEN)
#define MSOS20_SET_TOTAL (MSOS20_SET_HEADER_LEN + MSOS20_FUNCTION_TOTAL)

#define WBVAL(x) (uint8_t) ((x) & 0xFFU), (uint8_t) (((x) >> 8) & 0xFFU)
#define DBVAL(x)                                             \
    (uint8_t) ((x) & 0xFFU), (uint8_t) (((x) >> 8) & 0xFFU), \
        (uint8_t) (((x) >> 16) & 0xFFU), (uint8_t) (((x) >> 24) & 0xFFU)

#define USB_SERIAL_HEX_LEN (32U)

static char s_serial[USB_SERIAL_HEX_LEN + 1U];

/* ============================================================================
 * 设备 / 配置 / 字符串
 * ============================================================================ */
static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, /* BOS + MS OS 2.0 要求 ≥ 2.10 */
    .bDeviceClass = 0x00,    /* 类在接口层（0xFE/0x01/0x02），设备层不声明 */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = BOOT_USB_VID,
    .idProduct = BOOT_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1), 0x80, 500),
    TUD_DFU_DESCRIPTOR(0, 1, 4,
                       DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT, 1000,
                       CFG_TUD_DFU_XFER_BUFSIZE),
};

/* ============================================================================
 * MS OS 2.0 描述符集 + BOS
 * ============================================================================ */
static const uint8_t s_msos20_desc[] = {
    /* Set header */
    WBVAL(MSOS20_SET_HEADER_LEN),
    WBVAL(0x0000U), /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    DBVAL(0x06030000U), /* Windows 8.1+ */
    WBVAL(MSOS20_SET_TOTAL),

    /* Function subset header（bFirstInterface = 0） */
    WBVAL(MSOS20_FUNCTION_SUBSET_LEN),
    WBVAL(0x0002U), /* MS_OS_20_SUBSET_HEADER_FUNCTION */
    0x00U,
    0x00U,
    WBVAL(MSOS20_FUNCTION_TOTAL),

    /* Compatible ID: WINUSB */
    WBVAL(MSOS20_COMPAT_ID_LEN),
    WBVAL(0x0003U),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Registry property: DeviceInterfaceGUIDs = {CDB3B5AD-293B-4663-AA36-01AAE4646377} */
    WBVAL(MSOS20_REG_PROP_LEN),
    WBVAL(0x0004U),
    WBVAL(0x0007U), /* REG_MULTI_SZ */
    WBVAL(42U),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0,
    't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0,
    'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80U),
    '{', 0, 'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0,
    '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-', 0,
    '0', 0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0,
    '}', 0, 0, 0, 0, 0,
};

static const uint8_t s_bos_desc[] = {
    5U, TUSB_DESC_BOS, WBVAL(5U + 28U), 1U,
    /* MS OS 2.0 Platform Capability */
    28U, TUSB_DESC_DEVICE_CAPABILITY, 0x05U, 0x00U,
    /* UUID D8DD60DF-4589-4CC7-9CD2-659D9E648A9F（GUID little-endian 字节序） */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    DBVAL(0x06030000U),
    WBVAL(MSOS20_SET_TOTAL),
    MSOS20_VENDOR_CODE,
    0x00U,
};

_Static_assert(sizeof(s_msos20_desc) == MSOS20_SET_TOTAL, "MS OS 2.0 descriptor length mismatch");

/* ============================================================================
 * 序列号（OTP UUID）
 * ============================================================================ */
static void serial_from_otp(void) {
    static const char hex_digits[] = "0123456789ABCDEF";
    uint32_t uuid[OTP_SOC_UUID_LEN / sizeof(uint32_t)];
    bool blank = true;

    for (uint32_t i = 0U; i < (OTP_SOC_UUID_LEN / sizeof(uint32_t)); i++) {
        uuid[i] = otp_read_from_shadow(OTP_SOC_UUID_IDX + i);
        if ((uuid[i] != 0U) && (uuid[i] != 0xFFFFFFFFU)) {
            blank = false;
        }
    }

    if (blank) {
        (void) strcpy(s_serial, "G6618-UNPROGRAMMED");
        return;
    }

    for (uint32_t i = 0U; i < 4U; i++) {
        for (uint32_t j = 0U; j < 8U; j++) {
            s_serial[(i * 8U) + j] = hex_digits[(uuid[i] >> (28U - (4U * j))) & 0x0FU];
        }
    }
    s_serial[USB_SERIAL_HEX_LEN] = '\0';
}

/* ============================================================================
 * 描述符回调
 * ============================================================================ */
uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *) &s_device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return s_config_desc;
}

uint8_t const *tud_descriptor_bos_cb(void) {
    return s_bos_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t desc_str[1U + (USB_SERIAL_HEX_LEN + 1U)];
    static const char *const strings[] = {
        "Alliance HardwareGroup", /* 1 */
        "G6618 DFU",              /* 2 */
        NULL,                     /* 3 = serial（动态） */
        NULL,                     /* 4 = interface（动态：目标槽） */
    };
    const char *str;
    size_t count;

    (void) langid;

    if (index == 0U) {
        desc_str[1] = 0x0409U; /* en-US */
        count = 1U;
    } else {
        if (index > (sizeof(strings) / sizeof(strings[0]))) {
            return NULL;
        }
        if (index == 3U) {
            if (s_serial[0] == '\0') {
                serial_from_otp();
            }
            str = s_serial;
        } else if (index == 4U) {
            /* 接口名带目标槽，dfu-util -l 可直接看出该发哪个 bin */
            static char s_interface[] = "G6618 DFU (x)";
            const size_t slot_char = sizeof(s_interface) - 3U;

            s_interface[slot_char] = (char) ('A' + (dfu_target_slot() & 1U));
            str = s_interface;
        } else {
            str = strings[index - 1U];
        }

        count = strlen(str);
        if (count > USB_SERIAL_HEX_LEN) {
            count = USB_SERIAL_HEX_LEN;
        }
        for (size_t i = 0U; i < count; i++) {
            desc_str[1U + i] = (uint16_t) (uint8_t) str[i];
        }
    }

    desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2U * count + 2U));
    return desc_str;
}

/* ============================================================================
 * MS OS 2.0 厂商请求（bRequest = bVendorCode, wIndex = 7）
 * ============================================================================ */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if ((request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR)
        || (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_DEVICE)
        || (request->bRequest != MSOS20_VENDOR_CODE) || (request->wIndex != 0x0007U)) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    return tud_control_xfer(rhport, request, (void *) (uintptr_t) s_msos20_desc,
                            sizeof(s_msos20_desc));
}
