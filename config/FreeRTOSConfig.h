/**
 * @file    FreeRTOSConfig.h
 * @brief   FreeRTOS 内核配置（HPM SDK 默认 MCHTMR tick @24MHz）
 * @author  Kaiser
 *
 * 以 SDK samples/rtos/freertos/freertos_hello/src/FreeRTOSConfig.h 为底，
 * 针对本工程调整：
 *   - tick 源 = MCHTMR（portasmHAS_MTIME=1，SDK 默认），configCPU_CLOCK_HZ = 24MHz
 *     （MCHTMR 时钟，与 CPU 480MHz 无关）
 *   - heap 16KB（heap_4，落 .bss → DLM）；A 阶段仅 bring-up 任务，B 阶段增 rtt_log
 *   - configASSERT / 栈溢出 / malloc 失败 → app_rtos_fatal（RTT 报错 + 三相紧急关断 + 停机）
 *   - configUSE_TIMERS = 0（A/B 不需要软件定时器）
 *
 * 设计文档：docs/superpowers/specs/2026-09-23-rtos-foundation-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "board.h"

/* ---------------------------------------------------------------------------
 * tick 计时基址（MCHTMR 路径由 CMake 编译定义 portasmHAS_MTIME=1 选中）
 * ------------------------------------------------------------------------- */
#if (portasmHAS_MTIME == 0)
#define configMTIME_BASE_ADDRESS                (0)
#define configMTIMECMP_BASE_ADDRESS             (0)
#else
#define configMTIME_BASE_ADDRESS                (HPM_MCHTMR_BASE)
#define configMTIMECMP_BASE_ADDRESS             (HPM_MCHTMR_BASE + 8UL)
#endif

/* A/B 阶段不启用 USE_SYSCALL_INTERRUPT_PRIORITY：
 * critical section = 全局清 mstatus.MIE（简化模型）。
 * 约束：任何 ISR 不得调用 FreeRTOS API（现状满足）。 */

#define configUSE_PREEMPTION                    1
#define configCPU_CLOCK_HZ                      ((uint32_t) 24000000) /* MCHTMR = osc24m/1 */
#define configTICK_RATE_HZ                      ((TickType_t) 1000)
#define configMAX_PRIORITIES                    (32)
#define configMINIMAL_STACK_SIZE                (256)   /* idle，word */
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 0
#define configUSE_APPLICATION_TASK_TAG          0
#define configGENERATE_RUN_TIME_STATS           0

/* Memory allocation definitions. */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   ((size_t) (16 * 1024))

/* Hook function definitions. */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* Run time and task stats gathering definitions. */
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Set the following definitions to 1 to include the API function, or zero to exclude the API function. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources           1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/* Co-routine definitions. */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         2

/* ---------------------------------------------------------------------------
 * 同步原语 / 软件定时器
 *
 * 说明：CONFIG_FREERTOS 会让 CherryUSB 连带编译 osal/usb_osal_freertos.c，
 * 该文件引用 xSemaphoreCreateMutex / xSemaphoreCreateCounting / xTimerCreate。
 * 但 config/usb_config.h 中 CONFIG_USBDEV_EP0_THREAD 未定义，usbd_core.c 走
 * 裸机 #else 路径，运行时不会调用 usb_osal_* —— USB 行为零变化。
 * 这里打开 API 仅为满足该文件的编译与链接。
 * ------------------------------------------------------------------------- */
#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                8
/* port.c 的 vApplicationGetTimerTaskMemory 无条件编译，必须定义本宏 */
#define configTIMER_TASK_STACK_DEPTH            (256)

/* Normal assert() semantics without relying on the provision of an assert.h header file. */
#ifndef __ASSEMBLER__
void app_rtos_fatal(const char* file, long line);
#endif
#define configASSERT(x)                         \
    do {                                        \
        if ((x) == 0) {                         \
            app_rtos_fatal(__FILE__, __LINE__); \
        }                                       \
    } while (0)

/* Enable Hardware Stack Protection and Recording mechanism. */
#define configHSP_ENABLE                        0

#if (configHSP_ENABLE == 1 && configRECORD_STACK_HIGH_ADDRESS != 1)
#define configRECORD_STACK_HIGH_ADDRESS         1
#endif

#endif /* FREERTOS_CONFIG_H */
