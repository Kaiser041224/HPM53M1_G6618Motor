/**
 * @file    app_debug_rtt.c
 * @brief   SEGGER RTT 调试输出封装（RTT 写出由 rtt_log 任务调度）
 * @author  Kaiser
 *
 * 调度模型（设计文档 §2.2）：
 *   app_debug_printf → 同步 writer → 入队
 *   rtt_log 任务（阻塞等队列）→ SEGGER RTT 写出 + 1s 心跳（水位/丢弃计数）
 *
 * 优先级：rtt_log > app（APP_RTOS_PRIO_LOG=3 > APP_RTOS_PRIO_BRINGUP=2）。
 * app 超循环忙等不阻塞，低优先级 logger 会被饿死，故 log 必须更高优先级；
 * 无日志时 log 任务阻塞在队列上，对控制环零打扰。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_debug_rtt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "SEGGER_RTT.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_rtos.h"

/** 单条日志最大长度（与历史 app_debug_printf 行缓冲一致，避免新增截断） */
#define APP_DEBUG_RTT_MSG_MAX   (256)
/** 日志队列深度（条） */
#define APP_DEBUG_RTT_QUEUE_LEN (8)
/** 心跳周期（ms） */
#define APP_DEBUG_RTT_HB_MS     (1000U)

/** 旁路写入器（NULL = 仅 RTT） */
static app_debug_writer_t s_writer;
/** 日志队列（NULL = start_task 之前，printf 退化为直写） */
static QueueHandle_t s_log_queue;
/** RTT 写出成功条数（log 任务侧计数） */
static volatile uint32_t s_write_ok;
/** 入队失败丢弃条数（生产者侧计数） */
static volatile uint32_t s_write_dropped;

void app_debug_set_writer(app_debug_writer_t writer) {
    s_writer = writer;
}

/**
 * @brief rtt_log 任务体：排空队列写 RTT + 周期心跳。
 * @param arg 未使用
 */
static void app_debug_rtt_log_task(void* arg) {
    char buf[APP_DEBUG_RTT_MSG_MAX];
    uint32_t heartbeat = 0U;
    TickType_t last_hb = xTaskGetTickCount();

    (void)arg;

    for (;;) {
        if (xQueueReceive(s_log_queue, buf, pdMS_TO_TICKS(50)) == pdTRUE) {
            SEGGER_RTT_WriteString(0, buf);
            s_write_ok++;

            /* 一次唤醒尽量排空，减少上下文切换 */
            while (xQueueReceive(s_log_queue, buf, 0) == pdTRUE) {
                SEGGER_RTT_WriteString(0, buf);
                s_write_ok++;
            }
        }

        /* 心跳：证明调度在跑 + 暴露队列水位/丢弃计数 */
        if ((uint32_t)(xTaskGetTickCount() - last_hb) >= pdMS_TO_TICKS(APP_DEBUG_RTT_HB_MS)) {
            last_hb = xTaskGetTickCount();
            heartbeat++;
            SEGGER_RTT_printf(0, "rtt_hb=%u q=%u drop=%u\r\n", (unsigned)heartbeat,
                              (unsigned)uxQueueMessagesWaiting(s_log_queue),
                              (unsigned)s_write_dropped);
        }
    }
}

void app_debug_rtt_start_task(void) {
    s_log_queue = xQueueCreate(APP_DEBUG_RTT_QUEUE_LEN, APP_DEBUG_RTT_MSG_MAX);
    if (s_log_queue == NULL) {
        app_rtos_fatal(__FILE__, __LINE__);
    }

    if (xTaskCreate(app_debug_rtt_log_task, "rtt_log", APP_RTOS_STACK_LOG_WORDS, NULL,
                    APP_RTOS_PRIO_LOG, NULL) != pdPASS) {
        app_rtos_fatal(__FILE__, __LINE__);
    }
}

int app_debug_printf(const char* fmt, ...) {
    char buf[APP_DEBUG_RTT_MSG_MAX];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 旁路 writer 在生产者上下文同步调用（保持 Terminal capture 时序） */
    if (s_writer != NULL) {
        s_writer(buf, strlen(buf));
    }

    if (s_log_queue != NULL) {
        /* 满则丢弃整条，不阻塞生产者 */
        if (xQueueSend(s_log_queue, buf, 0) != pdTRUE) {
            s_write_dropped++;
        }
    } else {
        /* start_task 之前：直写，保证早期日志不丢 */
        SEGGER_RTT_WriteString(0, buf);
    }

    return len;
}

uint32_t app_debug_rtt_get_write_ok(void) {
    return s_write_ok;
}

uint32_t app_debug_rtt_get_write_dropped(void) {
    return s_write_dropped;
}

uint32_t app_debug_rtt_get_queue_depth(void) {
    return (s_log_queue != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_log_queue) : 0U;
}
