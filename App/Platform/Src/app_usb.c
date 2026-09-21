/**
 * @file    app_usb.c
 * @brief   USB CDC 虚拟串口平台封装（USB0，HS）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_usb.h"

#include "intf_usb_cdc.h"

#include <string.h>

/* Driver registration */
extern void hpm_usb_cdc_driver_register(void);

/* USB CDC 设备对象（init 时解析并缓存） */
static const intf_usb_cdc_t* s_usb;

void app_usb_init(void) {
    hpm_usb_cdc_driver_register();

    s_usb = intf_usb_cdc_get();
    if (s_usb == NULL) {
        return;
    }

    (void)s_usb->init();
}

int app_usb_write(const uint8_t* data, size_t len) {
    return app_usb_write_timeout(data, len, 100U);
}

int app_usb_write_timeout(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if (s_usb == NULL) {
        return -1;
    }
    return s_usb->write(data, len, timeout_ms);
}

int app_usb_write_str(const char* str) { return app_usb_write((const uint8_t*)str, strlen(str)); }

int app_usb_read(uint8_t* data, size_t len) {
    if (s_usb == NULL) {
        return -1;
    }
    return s_usb->read(data, len);
}

bool app_usb_is_dtr(void) {
    if (s_usb == NULL) {
        return false;
    }
    return s_usb->is_dtr();
}
