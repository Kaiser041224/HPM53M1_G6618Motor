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
extern void hpm_usb_bulk_driver_register(void);

/* USB 设备对象（init 时解析并缓存；CDC 与 bulk 共用同一次设备初始化） */
static const intf_usb_cdc_t* s_usb;
static const intf_usb_bulk_t* s_usb_bulk;

void app_usb_init(void) {
    hpm_usb_cdc_driver_register();
    hpm_usb_bulk_driver_register();

    s_usb = intf_usb_cdc_get();
    s_usb_bulk = intf_usb_bulk_get();
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

void app_usb_get_stats(intf_usb_cdc_stats_t* out) {
    if ((s_usb == NULL) || (s_usb->get_stats == NULL)) {
        return;
    }
    s_usb->get_stats(out);
}

bool app_usb_is_dtr(void) {
    if (s_usb == NULL) {
        return false;
    }
    return s_usb->is_dtr();
}

int app_usb_bulk_write(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if (s_usb_bulk == NULL) {
        return -1;
    }
    return s_usb_bulk->write(data, len, timeout_ms);
}

int app_usb_bulk_read(uint8_t* data, size_t len) {
    if (s_usb_bulk == NULL) {
        return -1;
    }
    return s_usb_bulk->read(data, len);
}

bool app_usb_bulk_ready(void) {
    if (s_usb_bulk == NULL) {
        return false;
    }
    return s_usb_bulk->is_ready();
}

void app_usb_bulk_get_stats(intf_usb_bulk_stats_t* out) {
    if ((s_usb_bulk == NULL) || (s_usb_bulk->get_stats == NULL)) {
        return;
    }
    s_usb_bulk->get_stats(out);
}
