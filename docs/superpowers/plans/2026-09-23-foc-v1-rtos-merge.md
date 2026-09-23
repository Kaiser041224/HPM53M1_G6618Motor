# FOC 电流环 × FreeRTOS 框架合并实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `feat/rtos-foundation` 的 FreeRTOS 框架合并进 `feat/foc-v1`，产出「FOC 电流环最小可测」镜像（辨识/V-F 编译级裁掉、保护动作全关），交付可上板的 M1 测试镜像。

**Architecture:** git 语义合并（方案 A）。9 个双改文件按 spec §3 裁决；快车道 = `app_fast_step_hook`（mcycle 计时包装）→ `app_foc_isr_step`（foc-v1 原样）；io(1ms)/diag(1s)/rtt_log 三任务取 rtos 侧；`app_motor_identify` / `id_encoder` / `app_debug_motor` / `algo_trig` 不参与构建，调用点换空桩。

**Tech Stack:** C17 / HPM SDK 1.12.1 / FreeRTOS（非向量 + 非抢占）/ CherryUSB / SEGGER RTT / 主机单测（cc，foc-v1 的 scripts/tests/*）

**Spec:** `docs/superpowers/specs/2026-09-23-foc-v1-rtos-merge-design.md`（已批准）

---

## 提交布局（2 个 commit）

1. **merge commit**（`git merge --no-ff feat/rtos-foundation`）：全部冲突裁决 + M1 裁剪 + 空桩 + 命令收敛。回滚：`git revert -m 1 <merge>`。
2. **fix(fault) commit**：`app_fault.c` OV/UV 由「无条件停机」改为「跟随 shutdown_en」（spec §6 保护逻辑唯一改动）。单独 revert 该 commit = 旋转测试前把 OV/UV 停机动作加回。

**合并期间（Task 1~9）工作树处于 merge-in-progress，所有修改累积在暂存区/工作树，只在 Task 9 末尾 `git commit` 一次**；Task 10 在合并提交之后做，独立成 commit。

**合并方向语义**：在 `feat/foc-v1` 上执行 `git merge feat/rtos-foundation` → `--ours` = foc-v1 侧，`--theirs` = rtos-foundation 侧。

**执行模式**：Kaiser 已指令「开始合并 / 不要擅自中断」→ 按 inline 顺序连续执行（superpowers:executing-plans），不在中途暂停询问。

---

## Task 1: 合并发起 + 冲突集核对

**Files:** 无修改（只读验证 + git 操作）

- [ ] **Step 1: 记录回滚点、切到目标分支**

```bash
cd /workspace/projects/HPM53M1_G6618Motor
git status --short && git rev-parse HEAD
git checkout feat/foc-v1 && git rev-parse HEAD
```
Expected: 工作树干净；记录两个 SHA（`<ROLLBACK_A>` = rtos-foundation HEAD，`<ROLLBACK_B>` = foc-v1 合并前 HEAD，回滚用 `git reset --hard <ROLLBACK_B>`）。

- [ ] **Step 2: 跑 foc-v1 基线主机测试（绿基线门槛）**

```bash
bash scripts/tests/foc/run.sh 2>&1 | tail -3
bash scripts/tests/control/run.sh 2>&1 | tail -3
bash scripts/tests/encoder/run.sh 2>&1 | tail -3
```
Expected: 三脚本 PASS（各自两遍编译：-O1 与 -O2 -ffast-math）。若基线红：停，报告，不合并。

- [ ] **Step 3: 发起合并**

```bash
git merge --no-ff feat/rtos-foundation
git status --short | grep -E '^(UU|AA|DU|UD|AU|UA)' || true
```
Expected: merge 报自动合并失败（正常）；冲突集 ⊆ {`AGENTS.md`, `App/Debug/Inc/app_debug_encoder.h`, `App/Debug/Src/app_debug_encoder.c`, `App/Debug/Src/app_debug_motor.c`, `App/Logic/app_logic.c`, `App/Platform/Inc/app_adc.h`, `App/Platform/Src/app_adc.c`, `CMakeLists.txt`, `Driver/hpm_impl/drv_hrpwm.c`}。`AGENTS.md`/`drv_hrpwm.c` 双改区域相隔远，可能被自动合并 → Task 6 对两者都做「验证或裁决」。

- [ ] **Step 4: 核对单侧文件已自动并入**

```bash
git diff --cached --name-only | grep -E 'app_rtos|FreeRTOSConfig|app_debug_rtt|drv_adc|app_foc|app_motor_identify|cmd_foc|gen_params' || true
```
Expected: 含 `App/Platform/Src/app_rtos.c`、`App/Platform/Inc/app_rtos.h`、`App/Logic/app_rtos_tasks.c`、`config/FreeRTOSConfig.h`、`App/Debug/Src/app_debug_rtt.c`、`Driver/hpm_impl/drv_adc.c`、`App/Control/Src/app_foc.c`、`App/Control/Src/app_foc_current.c`、`App/Comm/terminal/Src/app_terminal_cmd_foc.c`。

---

## Task 2: CMakeLists.txt 重写

**Files:**
- Rewrite: `CMakeLists.txt`

**裁决要点**（spec §3）：rtos 的 FreeRTOS 块 + foc-v1 的源列表组织；剔除 `app_motor_identify.c` / `id_encoder.c` / `app_debug_motor.c`；**不**加入 `algo_trig.c`；补 `app_rtos.c` / `app_rtos_tasks.c`。

- [ ] **Step 1: 整文件替换 `CMakeLists.txt` 为以下内容**

```cmake
# CMakeLists.txt
# Copyright (c) 2024 HPMicro
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============================================================================
# CherryUSB middleware (USB Device + CDC ACM)
# 必须在 find_package(hpm-sdk) 之前设置，供 middleware/CMakeLists.txt 判定
# ============================================================================

set(CONFIG_CHERRYUSB 1)
set(CONFIG_USB_DEVICE 1)
set(CONFIG_USB_DEVICE_CDC_ACM 1)

# ============================================================================
# FreeRTOS middleware（tick 源 = SDK 默认 MCHTMR @24MHz，见 config/FreeRTOSConfig.h）
# CONFIG_FREERTOS 必须在 find_package(hpm-sdk) 之前设置，供 middleware/CMakeLists.txt 判定
# ============================================================================

set(CONFIG_FREERTOS 1)

# Linker scripts
set(CUSTOM_GCC_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/gcc/user_linker.ld)
set(CUSTOM_SES_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/segger/user_linker.icf)
set(CUSTOM_IAR_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/iar/user_linker.icf)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

project(user_app)

# ============================================================================
# FreeRTOS 中断模式（对齐 HPM SDK 官方 FreeRTOS 示例惯例，57/57 示例全部如此）
#   - USE_NONVECTOR_MODE=1：走 freertos_handle_interrupt + HPM_PLIC_BASE 宏做
#     PLIC claim/complete（地址正确）。向量模式的 freertos_hpmicro_vectors.h
#     硬编码 PLIC complete 到 0xE4200000，而 HPM5361 实际 PLIC_BASE=0xE4000000，
#     差 2MB —— 每个 ISR 结束都写错地址，25kHz ADC 中断密集触发时总线异常。
#     该 bug 仅存在于向量模式路径，官方 57/57 示例均用非向量模式故从未暴露。
#   - DISABLE_IRQ_PREEMPTIVE=1：与 55/57 官方示例一致（非向量 + 非抢占组合经官方验证）。
#     AGENTS.md「FreeRTOS 引入纪律」为权威说明，勿改为向量模式。
# ============================================================================

sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)
sdk_compile_definitions(-DDISABLE_IRQ_PREEMPTIVE=1)

# 数学库（sinf/cosf 等：foc_math 查表初始化、模拟量换算）
sdk_link_libraries(m)

# ============================================================================
# 参数管线（config/*.yaml → build/generated；configure 期生成，失败即中止）
# ============================================================================

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PARAMS_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_params.py
            --config-dir ${CMAKE_CURRENT_SOURCE_DIR}/config --out-dir ${PARAMS_GEN_DIR}
    RESULT_VARIABLE _params_rc
    ERROR_VARIABLE _params_err)
if(NOT _params_rc EQUAL 0)
    message(FATAL_ERROR "参数生成失败（退出码 ${_params_rc}）：\n${_params_err}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/config/motor.yaml
    ${CMAKE_CURRENT_SOURCE_DIR}/config/hardware.yaml
    ${CMAKE_CURRENT_SOURCE_DIR}/config/software.yaml
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_params.py)
sdk_app_inc(${PARAMS_GEN_DIR})
sdk_app_src(${PARAMS_GEN_DIR}/params_generated.c)
sdk_app_src(${PARAMS_GEN_DIR}/params_meta_generated.c)

# ============================================================================
# CherrySH（Shell 核心）+ cherryrb（环形缓冲）
# 不使用 SDK port（barebone_usb/shell.c 的 CDC 描述符/回调与 drv_usb_cdc.c 冲突）；
# 不设置 CONFIG_CHERRYSH，直接引用源文件（同 SEGGER RTT 方式）。
# ============================================================================

set(CHERRYSH_DIR $ENV{HPM_SDK_BASE}/middleware/cherrysh)
set(CHERRYRB_DIR $ENV{HPM_SDK_BASE}/middleware/cherryrb)
sdk_inc(${CHERRYSH_DIR})
sdk_inc(${CHERRYRB_DIR})
sdk_app_src(${CHERRYSH_DIR}/chry_shell.c)
sdk_app_src(${CHERRYSH_DIR}/builtin/help.c)
sdk_app_src(${CHERRYRB_DIR}/chry_ringbuffer.c)

# ============================================================================
# SEGGER RTT (manually included, no syscalls)
# ============================================================================

set(RTT_DIR $ENV{HPM_SDK_BASE}/middleware/segger_rtt)
sdk_app_inc(${RTT_DIR}/Config)
sdk_app_inc(${RTT_DIR}/RTT)
sdk_app_src(${RTT_DIR}/RTT/SEGGER_RTT.c)
sdk_app_src(${RTT_DIR}/RTT/SEGGER_RTT_printf.c)
sdk_inc(${RTT_DIR}/Config)
sdk_inc(${RTT_DIR}/RTT)

# ============================================================================
# CherryUSB 配置（usb_config.h，middleware 源文件也需要该路径）
# ============================================================================

sdk_inc(${CMAKE_CURRENT_SOURCE_DIR}/config)

# ============================================================================
# Include Paths
# ============================================================================

sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/Interface)

sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Logic)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Algorithm/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Algorithm/FOC/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Platform/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Control/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Comm/terminal/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Comm/can/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Debug/Inc)

sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/Driver/hpm_impl)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/Driver/IrqProfiler)

if(DEFINED ENV{HPMDEV_SHARED_DIR} AND EXISTS "$ENV{HPMDEV_SHARED_DIR}/include")
    sdk_app_inc($ENV{HPMDEV_SHARED_DIR}/include)
endif()

# ============================================================================
# Driver Layer (SDK adapters)
# ============================================================================

sdk_app_src(Driver/hpm_impl/drv_clock.c)
sdk_app_src(Driver/hpm_impl/drv_sys.c)
sdk_app_src(Driver/hpm_impl/drv_uart.c)
sdk_app_src(Driver/hpm_impl/drv_usb_cdc.c)
sdk_app_src(Driver/hpm_impl/drv_spi.c)
sdk_app_src(Driver/encoder/drv_kth7823.c)
sdk_app_src(Driver/hpm_impl/drv_flash.c)
sdk_app_src(Driver/hpm_impl/drv_gpio.c)
sdk_app_src(Driver/hpm_impl/drv_hrpwm.c)
sdk_app_src(Driver/hpm_impl/drv_gptmr.c)
sdk_app_src(Driver/hpm_impl/drv_adc.c)
sdk_app_src(Driver/hpm_impl/drv_trgm.c)
sdk_app_src(Driver/hpm_impl/drv_synt.c)
sdk_app_src(Driver/hpm_impl/drv_mcan.c)
sdk_app_src(Driver/IrqProfiler/irq_profiler.c)

# ============================================================================
# Interface Layer
# ============================================================================

sdk_app_src(Interface/intf_default.c)

# ============================================================================
# Algorithm Library (pure C, hardware independent)
#   M1 裁剪：id_encoder.c（辨识专用）不编译；algo_trig.c（V/F 专用）不编译。
# ============================================================================

sdk_app_src(App/Algorithm/Src/algo_ffd.c)
sdk_app_src(App/Algorithm/Src/algo_filter.c)
sdk_app_src(App/Algorithm/Src/algo_hyst.c)
sdk_app_src(App/Algorithm/Src/algo_pid.c)
sdk_app_src(App/Algorithm/Src/algo_pll.c)
sdk_app_src(App/Algorithm/Src/algo_ramp.c)
sdk_app_src(App/Algorithm/Src/algo_rms.c)
sdk_app_src(App/Algorithm/Src/algo_encoder_snapshot.c)
sdk_app_src(App/Algorithm/FOC/Src/foc_math.c)
sdk_app_src(App/Algorithm/FOC/Src/foc_angle.c)
sdk_app_src(App/Algorithm/FOC/Src/foc_current.c)
sdk_app_src(App/Algorithm/FOC/Src/foc_modulation.c)

# ============================================================================
# Platform Layer
# ============================================================================

sdk_app_src(App/Platform/Src/app_gpio.c)
sdk_app_src(App/Platform/Src/app_uart.c)
sdk_app_src(App/Platform/Src/app_usb.c)
sdk_app_src(App/Platform/Src/app_encoder.c)
sdk_app_src(App/Platform/Src/app_3phase_inverter.c)
sdk_app_src(App/Platform/Src/app_param.c)
sdk_app_src(App/Platform/Src/app_hrpwm.c)
sdk_app_src(App/Platform/Src/app_gptmr.c)
sdk_app_src(App/Platform/Src/app_adc.c)
sdk_app_src(App/Platform/Src/app_analog_signal.c)
sdk_app_src(App/Platform/Src/app_can.c)
sdk_app_src(App/Platform/Src/app_hardware_params.c)
sdk_app_src(App/Platform/Src/app_software_params.c)
sdk_app_src(App/Platform/Src/app_rtos.c)

# ============================================================================
# Control Layer
#   M1 裁剪：app_motor_identify.c 不编译（调用点空桩见 Task 7）。
# ============================================================================

sdk_app_src(App/Control/Src/app_fault.c)
sdk_app_src(App/Control/Src/app_motor_params.c)
sdk_app_src(App/Control/Src/app_foc_current.c)
sdk_app_src(App/Control/Src/app_foc.c)

# ============================================================================
# Comm Layer
# ============================================================================

sdk_app_src(App/Comm/terminal/Src/app_terminal.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_job.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_sys.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_diag.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_motor.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_foc.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_param.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_monitor.c)

# ============================================================================
# Debug Layer
#   M1 裁剪：app_debug_motor.c 不编译（V/F，调用点清理见 Task 8）。
# ============================================================================

sdk_app_src(App/Debug/Src/app_debug_uart.c)
sdk_app_src(App/Debug/Src/app_debug_encoder.c)
sdk_app_src(App/Debug/Src/app_debug_cmd.c)
sdk_app_src(App/Debug/Src/app_debug_flash.c)
sdk_app_src(App/Debug/Src/app_debug_inverter.c)
sdk_app_src(App/Debug/Src/app_debug_can.c)
sdk_app_src(App/Debug/Src/app_debug_adc.c)
sdk_app_src(App/Debug/Src/app_debug_fault.c)
sdk_app_src(App/Debug/Src/app_debug_foc.c)
sdk_app_src(App/Debug/Src/app_debug_hrpwm.c)
sdk_app_src(App/Debug/Src/app_debug_profiler.c)
sdk_app_src(App/Debug/Src/app_debug_rtt.c)

# ============================================================================
# Application
# ============================================================================

sdk_app_src(App/Logic/app_logic.c)
sdk_app_src(App/Logic/app_rtos_tasks.c)
sdk_app_src(App/main.c)

# ============================================================================

generate_ide_projects()
```

- [ ] **Step 2: 验证裁剪与 FreeRTOS 块就位**

```bash
grep -c 'app_motor_identify.c\|id_encoder.c\|app_debug_motor.c\|algo_trig.c' CMakeLists.txt
grep -n 'USE_NONVECTOR_MODE\|DISABLE_IRQ_PREEMPTIVE\|CONFIG_FREERTOS' CMakeLists.txt
```
Expected: 第一条输出 `0`（四个被裁源文件都不出现在构建列表；注释里提到的除外——注释文本含名字，故 grep 计数可能非 0，人工确认无 `sdk_app_src` 行引用它们即可，用 `grep 'sdk_app_src.*\(app_motor_identify\|id_encoder\|app_debug_motor\|algo_trig\)' CMakeLists.txt` 应无输出）；第二条列出 3 处（`set(CONFIG_FREERTOS 1)`、两条 `sdk_compile_definitions`）。

- [ ] **Step 3: 验证补齐的 rtos 源**

```bash
grep -n 'app_rtos.c\|app_rtos_tasks.c\|app_debug_rtt.c' CMakeLists.txt
```
Expected: 3 行命中（Platform 的 `app_rtos.c`、Application 的 `app_rtos_tasks.c`、Debug 的 `app_debug_rtt.c`）。

---

## Task 3: App/Logic/app_logic.c 重写

**Files:**
- Rewrite: `App/Logic/app_logic.c`

**裁决要点**（spec §3/§5）：rtos 骨架（step 函数 + hook + [ISR] 统计）+ foc-v1 内容（init 序列 + `app_foc_isr_step` 调用）。`app_foc_init()` 自注册 `app_foc_isr_step` 为 current hook；本文件在 init 末尾**重新注册 `app_fast_step_hook`**（mcycle 计时包装，覆盖直注册）。`intf_clock_init()` 已在 `main()`（rtos 侧 main.c 自动并入），init 内不再调用。`app_debug_motor_init()` 裁掉。foc-v1 的 25kHz 心跳打印（`APP_DEBUG_PERIODIC_PRINT`，默认 0=关）不迁移——M1 观测面 = Terminal + Ozone + RTT `[ISR]`/`[ENC]`（spec §7.1）。

- [ ] **Step 1: 整文件替换 `App/Logic/app_logic.c` 为以下内容**

```c
/**
 * @file    app_logic.c
 * @brief   初始化编排 + FOC 快车道钩子 / IO / 诊断步骤函数
 * @author  Kaiser
 *
 * 步骤函数（任务编排见 App/Logic/app_rtos_tasks.c）：
 *   - app_init()           ：一次性串行初始化（调度器后由 app_io 任务执行，此时 ISR 可用）
 *   - app_fast_step_hook() ：FOC 硬实时快车道（25kHz，ADC PMT 完成中断内）
 *                            mcycle 计时包装 → app_foc_isr_step
 *                            硬约束：无 RTOS API、无 printf、无动态分配、无等待
 *   - app_io_step()        ：RTOS 后台 IO（1ms）——模拟量/故障/状态机编排 + 通讯轮询
 *   - app_diag_step()      ：RTOS 后台诊断（1s）——回报（LED / 统计），不改控制状态
 *
 * 时钟：intf_clock_init() 在 main()、调度器启动前完成（对齐 SDK 惯例，
 *      保证 MCHTMR tick 基频正确）。设计文档：
 *      docs/superpowers/specs/2026-09-23-foc-v1-rtos-merge-design.md
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_adc.h"
#include "app_analog_signal.h"
#include "app_debug_adc.h"
#include "app_debug_can.h"
#include "app_debug_encoder.h"
#include "app_debug_flash.h"
#include "app_debug_foc.h"
#include "app_debug_inverter.h"
#include "app_debug_rtt.h"
#include "app_debug_uart.h"
#include "app_encoder.h"
#include "app_fault.h"
#include "app_foc.h"
#include "app_gpio.h"
#include "app_gptmr.h"
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
#include "app_terminal.h"
#include "app_usb.h"
#include "intf_adc.h"
#include "intf_clock.h"
#include "intf_sys.h"

/* 前向声明：hook 注册于 app_init 末尾（覆盖 app_foc_init 的自注册），定义在后 */
static void app_fast_step_hook(void);

/* ============================================================================
 * ISR 耗时统计
 *   step 段 = app_foc_isr_step（FOC 控制段，hook 内测量）
 *   isr 段  = adc_generic_isr 整体（drv_adc 内测量：PMT 校验 + 控制段回调）
 * ISR 内只做廉价 min/max/累加；app_diag_step 每秒打印并清零窗口。
 * ========================================================================== */
static uint32_t s_step_min_cy = 0xFFFFFFFFU;
static uint32_t s_step_max_cy;
static uint32_t s_step_sum_cy;
static uint32_t s_step_cnt;

void app_init(void) {
    /*
     * 0. 复位诊断：
     *    - s_boot_seq 位于 NOLOAD 段（复位不清零）：逐次递增 = 芯片在复位循环。
     *    - rst_status 为 PPOR.RESET_STATUS（只读）：bit0=欠压、bit4=调试复位、
     *      bit16/17=看门狗、bit24=PMIC 看门狗、bit31=软件复位。
     */
    static volatile uint32_t s_boot_seq __attribute__((section(".noncacheable")));
    uint32_t rst_status;

    s_boot_seq++;
    rst_status = intf_sys_get_reset_status();
    app_debug_printf(
        "boot: seq=%u rst_status=0x%08x\r\n", (unsigned)s_boot_seq, (unsigned)rst_status);

    /* 0b. 参数单例初始化 + 摘要（验证 YAML 参数管线端到端） */
    {
        app_motor_params_init();
        app_hardware_params_init();
        app_software_params_init();

        const app_motor_params_t *motor_params = app_motor_params_current();
        const app_hardware_params_t *hardware = app_hardware_params_current();
        const app_software_params_t *software = app_software_params_current();

        app_debug_printf(
            "params: pp=%u rs=%.4f ls=%g | a/v=%.4f vbus/v=%.4f | oc=%.1f ov=%.1f uv=%.1f | "
            "pwm=%u/%u\r\n",
            (unsigned)motor_params->pole_pairs, (double)motor_params->rs_ohm,
            (double)motor_params->ls_h, (double)hardware->current_sense.a_per_volt,
            (double)hardware->vbus_sense.v_per_volt, (double)software->fault.oc_trip_a,
            (double)software->fault.vbus_ov_v, (double)software->fault.vbus_uv_v,
            (unsigned)hardware->inverter.pwm_freq_hz, (unsigned)hardware->inverter.deadtime_ns);
    }

    /* 1. 系统时钟已在 main() 中、调度器启动前完成（intf_clock_init）：
     *    CPU 480MHz / AXI-AHB 160MHz / PLL0 960MHz / DCDC 1275mV */

    /* 2. GPIO 驱动注册 + PA09（DRV_+12V_EN，默认输出低=关闭） */
    app_gpio_init();

    /* 3. 频率自检（寄存器读数） */
    app_debug_printf(
        "clock: cpu=%u Hz, ahb=%u Hz\r\n", (unsigned)intf_clock_get_cpu_freq(),
        (unsigned)intf_clock_get_ahb_freq());

    /* 4. UART0 自检（PA00/PA01，115200 8N1） */
    app_debug_uart_init();

    /* 5. CAN 自检（MCAN3，经典 CAN） */
    app_debug_can_init();

    /* 6. USB 终端（USB0 CDC 虚拟串口；CherrySH Terminal，1kHz 慢任务轮询） */
    app_usb_init();
    app_terminal_init();

    /* 7. 编码器自检（双 KTH7823：SPI3 转子 / SPI1 出轴） */
    app_debug_encoder_init();

    /* 7b. GPTMR 外设 + 12.5kHz 转子采样器（GPTMR1 CH3，PLIC prio 3 > ADC0）。
     *     此后 SPI3 归采样 ISR 独占，运行期读 API 只返回一致快照。 */
    app_gptmr_init();
    {
        int sampler_rc = app_encoder_sampler_start();

        app_debug_printf("[ENC] sampler: %s (GPTMR1 CH3 @12.5kHz)\r\n",
                         (sampler_rc == 0) ? "OK" : "FAILED");
    }

    /* 8. Flash 自检（XPI NOR：属性 + 末尾扇区破坏性读写测试） */
    app_debug_flash_init();

    /* 9. 三相半桥输出自检（PWM1：默认频率 / 50% 持续输出） */
    app_debug_inverter_init();

    /* 10. 故障保护（在 ADC 之前：提供 WDOG 阈值与回调） */
    app_fault_init(NULL);

    /* 11. ADC 采样链（PWM1 CMP10 触发 → TRGM → 双 ADC PMT：三相电流 + 母线/NTC；
     *     ADC0 电流通道启用 WDOG；ADC1 序列完成回调接入故障 tick @1kHz） */
    {
        app_adc_cfg_t adc_cfg;
        uint16_t wdog_hi;
        uint16_t wdog_lo;

        memset(&adc_cfg, 0, sizeof(adc_cfg));
        app_fault_get_wdog_raw(&wdog_hi, &wdog_lo);
        adc_cfg.wdog_en = true;
        adc_cfg.wdog_thshd_high = wdog_hi;
        adc_cfg.wdog_thshd_low = wdog_lo;
        adc_cfg.wdog_cb = app_fault_on_wdog;
        adc_cfg.wdog_cb_user = NULL;
        adc_cfg.slow_cb = app_fault_tick;
        app_adc_init(&adc_cfg);
    }
    app_analog_signal_init();
    app_debug_adc_init();

    /* 12. 电流零点标定（须在无电流状态：桥臂零矢量且未旋转） */
    if (app_analog_signal_calibrate_offsets() == 0) {
        app_debug_printf("[ADC] zero calibration: OK\r\n");
    } else {
        app_debug_printf("[ADC] zero calibration: FAILED (default 1.65V in use)\r\n");
    }

    /* 13. FOC（电流环编排；上电 OFF，不使能输出） */
    app_foc_init();

    /* 13b. FOC 调试观测/命令（Ozone 结构；io 任务有界处理请求） */
    app_debug_foc_init();

    /* 13c. 快车道钩子：覆盖 app_foc_init 内的直注册，注入 mcycle 计时包装 */
    app_adc_register_current_hook(app_fast_step_hook);
}

/**
 * @brief FOC 快车道钩子（ADC0 PMT 完成中断内调用，25kHz 硬件触发）。
 *
 * 节拍由 PWM1 CMP10 → TRGM → ADC0 PMT 保证；ISR 不被 RTOS 任务/日志抢占。
 * 仅做 mcycle 计时包装 + 耗时统计；控制语义全在 app_foc_isr_step（foc-v1 原样）。
 * 超预算（> 1 个节拍周期）记 late，供 [ENC]/[ISR] 统计。
 */
static void app_fast_step_hook(void) {
    static uint32_t s_budget_cycles;
    uint32_t start_cycle;
    uint32_t elapsed;

    if (s_budget_cycles == 0U) {
        s_budget_cycles = intf_clock_get_cpu_freq()
                        / app_hardware_params_current()->inverter.pwm_freq_hz;
    }

    start_cycle = intf_clock_get_cycle();
    app_foc_isr_step();
    elapsed = intf_clock_get_cycle() - start_cycle;

    /* step 段耗时统计（1s 窗口；app_diag_step 读取并清零） */
    s_step_cnt++;
    s_step_sum_cy += elapsed;
    if (elapsed > s_step_max_cy) {
        s_step_max_cy = elapsed;
    }
    if (elapsed < s_step_min_cy) {
        s_step_min_cy = elapsed;
    }

    /* ISR 执行时间超过一个节拍周期（丢拍风险） */
    if (elapsed > s_budget_cycles) {
        app_debug_encoder_note_loop_late(elapsed - s_budget_cycles);
    }
}

void app_io_step(void) {
    /* RTOS 后台 IO 单步（1ms，由 app_io 任务调用）：单次处理有界（≤200µs 约束不变）。
     * 顺序：观测刷新 → 模拟量换算 → 故障 RMS → 状态机编排 → 通讯轮询。 */

    /* 转子观测变量刷新（缓存读，零 SPI 成本；出轴采样已裁，spec §3） */
    app_debug_encoder_sample();

    /* 模拟量：ADC 缓存 → 物理量换算 + 滤波（FOC 的 v_bus 亦取自本模块缓存） */
    app_analog_signal_process();

    /* 故障保护 L2/L3：RMS 累加 + 去抖判断（M1 动作全关、仅检测/告警，spec §6） */
    app_fault_process();

    /* FOC 状态机 / 给定编排（控制环在 ISR，见 app_foc_isr_step） */
    app_foc_run_once();

    /* Ozone 命令消费 + 状态域刷新（有界：≤2 条命令/拍） */
    app_debug_foc_tick();

    /* ADC1 慢速通道（VBUS/NTC/CANID 原始码生产者） */
    app_adc_slow_process();

    /* Ozone 观测变量刷新（.noncacheable，1ms 刷新，容忍撕裂） */
    app_debug_adc_update();

    /* 调试与通讯轮询（各自内部 1Hz 打印门限） */
    app_debug_uart_run_once();
    app_debug_can_run_once();
    app_terminal_run_once(); /* USB Terminal（命令执行 + 输入 + job tick + TX flush） */
}

void app_diag_step(void) {
    /* RTOS 后台诊断单步（1s，由 app_diag 任务调用）：纯回报，不改控制状态。 */
    app_gpio_toggle(PIN_LED_STATUS);
    app_debug_encoder_print_stats();
    app_isr_stats_print();
}

/**
 * @brief ISR 耗时统计打印（1s 窗口，由 app_diag_step 调用）
 *
 * 输出（单位 µs，一位小数）：
 *   step min/avg/max = app_foc_isr_step（FOC 控制段）
 *   isr  avg/peak    = adc_generic_isr 整体（PMT 校验 + 控制段；peak 每窗复位）
 *   headroom         = 40µs 节拍预算 − isr peak
 *   load             = isr 段 CPU 占用率（窗口内 delta，0.1% 单位）
 * 注：isr 段不含 FreeRTOS 上下文保存/恢复与 PLIC claim/complete（另约 1–2µs）。
 */
void app_isr_stats_print(void) {
    static uint64_t s_prev_isr_total;
    static uint32_t s_prev_isr_cnt;
    static uint32_t s_prev_cycle;
    static bool s_inited;

    intf_adc_diag_snapshot_t snap;
    uint32_t cycle_now;
    uint32_t window_cy;
    uint32_t div;
    uint32_t step_min;
    uint32_t step_avg;
    uint32_t step_max;
    uint32_t isr_avg;
    uint32_t isr_peak;
    uint32_t isr_cnt;
    uint32_t load_x10;
    uint32_t budget_cy;
    uint32_t headroom_cy;
    uint32_t status;

    /* 临界区快照 + 清零 step 统计（防止 ISR 并发写导致读数撕裂） */
    status = intf_sys_irq_save();
    step_min = s_step_min_cy;
    step_max = s_step_max_cy;
    step_avg = (s_step_cnt != 0U) ? (s_step_sum_cy / s_step_cnt) : 0U;
    s_step_min_cy = 0xFFFFFFFFU;
    s_step_max_cy = 0U;
    s_step_sum_cy = 0U;
    isr_cnt = s_step_cnt;
    s_step_cnt = 0U;
    intf_sys_irq_restore(status);

    if (intf_adc_get_diag_snapshot(&snap) != 0) {
        app_debug_printf("[ISR] diag unavailable\r\n");
        return;
    }

    cycle_now = intf_clock_get_cycle();
    window_cy = s_inited ? (uint32_t)(cycle_now - s_prev_cycle) : 0U;
    div = intf_clock_get_cpu_freq() / 1000000U; /* cycles per µs */
    if (div == 0U) {
        div = 1U;
    }
    budget_cy = (intf_clock_get_cpu_freq()
                 / app_hardware_params_current()->inverter.pwm_freq_hz);

    /* isr 段窗口均值（ADC0 = FOC 快车道中断）与本窗峰值 */
    isr_avg = 0U;
    load_x10 = 0U;
    if (s_inited && (isr_cnt > 0U)) {
        uint64_t delta_total = snap.isr_total_cycles[0] - s_prev_isr_total;
        uint32_t delta_cnt = snap.irq_entry[0] - s_prev_isr_cnt;

        if (delta_cnt > 0U) {
            isr_avg = (uint32_t)(delta_total / delta_cnt);
        }
        if (window_cy > 0U) {
            /* 0.1% 单位：占用率 × 1000（此前误用 ×10，读数偏小 100 倍） */
            load_x10 = (uint32_t)((delta_total * 1000ULL) / window_cy);
        }
    }
    isr_peak = snap.isr_cycles_max[0];
    headroom_cy = (budget_cy > isr_peak) ? (budget_cy - isr_peak) : 0U;

    app_debug_printf(
        "[ISR] n=%u | step min/avg/max=%u.%u/%u.%u/%u.%u | isr avg=%u.%u peak=%u.%u | "
        "budget=%u.%u headroom=%u.%u | load=%u.%u%%\r\n",
        (unsigned)isr_cnt, (unsigned)(step_min / div), (unsigned)((step_min % div) * 10U / div),
        (unsigned)(step_avg / div), (unsigned)((step_avg % div) * 10U / div),
        (unsigned)(step_max / div), (unsigned)((step_max % div) * 10U / div),
        (unsigned)(isr_avg / div), (unsigned)((isr_avg % div) * 10U / div),
        (unsigned)(isr_peak / div), (unsigned)((isr_peak % div) * 10U / div),
        (unsigned)(budget_cy / div), (unsigned)((budget_cy % div) * 10U / div),
        (unsigned)(headroom_cy / div), (unsigned)((headroom_cy % div) * 10U / div),
        (unsigned)(load_x10 / 10U), (unsigned)(load_x10 % 10U));

    /* 峰值按窗复位：否则开机瞬态（如标定期）污染 headroom 至关机 */
    intf_adc_reset_diag_max();

    s_prev_isr_total = snap.isr_total_cycles[0];
    s_prev_isr_cnt = snap.irq_entry[0];
    s_prev_cycle = cycle_now;
    s_inited = true;
}
```

- [ ] **Step 2: 验证关键接缝**

```bash
grep -n 'app_adc_register_current_hook\|app_foc_isr_step\|app_foc_run_once\|intf_clock_init\|app_debug_motor' App/Logic/app_logic.c
```
Expected: 命中 `app_adc_register_current_hook(app_fast_step_hook)`、`app_foc_isr_step()`（hook 内）、`app_foc_run_once()`（io 内）；**无** `intf_clock_init` 调用（仅注释提及）、**无** `app_debug_motor` 任何引用。

---

## Task 4: app_adc.{c,h} 裁决（--ours = foc-v1 的 current_hook 缝）

**Files:**
- Resolve: `App/Platform/Inc/app_adc.h`、`App/Platform/Src/app_adc.c`

**裁决要点**（spec §3）：保留 foc-v1 的 `app_adc_register_current_hook()` 缝（`void (*hook)(void)`）；rtos 的 `fast_cb`/`fast_cb_user` cfg 字段**不要**（计时语义已并入 Task 3 的 hook 包装）。

- [ ] **Step 1: 取 foc-v1 侧**

```bash
git checkout --ours App/Platform/Inc/app_adc.h App/Platform/Src/app_adc.c
git add App/Platform/Inc/app_adc.h App/Platform/Src/app_adc.c
```

- [ ] **Step 2: 验证缝存在且 fast_cb 消失**

```bash
grep -n 'app_adc_register_current_hook\|fast_cb\|s_current_hook' App/Platform/Inc/app_adc.h App/Platform/Src/app_adc.c
```
Expected: `app_adc_register_current_hook` 在 .h（声明）与 .c（定义）各 1 处；`s_current_hook` 在 .c ≥2 处（定义 + pmt_cb 内调用）；**无** `fast_cb`。

---

## Task 5: app_debug_encoder.{c,h} 裁决 + 出轴采样手术

**Files:**
- Resolve + Modify: `App/Debug/Inc/app_debug_encoder.h`、`App/Debug/Src/app_debug_encoder.c`

**裁决要点**（spec §3）：取 foc-v1 侧（seqlock 快照 + health 自检 API + `note_loop_late`——foc-v1 已含迟拍接口，无需从 rtos 并入）；**移除出轴采样入口**（`encoder_sample_output` / `ratio_accumulate` / 1/25 分频块），转子观测刷新保留。

- [ ] **Step 1: 取 foc-v1 侧**

```bash
git checkout --ours App/Debug/Inc/app_debug_encoder.h App/Debug/Src/app_debug_encoder.c
```
注：头文件**零修改**（`g_enc_output_*`/`g_enc_ratio_x10000` 声明保留，定义保留但不再更新——Ozone 布局稳定）。

- [ ] **Step 2: app_debug_encoder.c —— 删除出轴采样机器**

删除 `#define ENC_OUTPUT_SAMPLE_DIV (25U)` 及其注释行（\"出轴编码器降采样…\"）。

删除整段游标比值累计（含 banner 注释、6 个 `s_ratio_*` 静态变量、`ratio_accumulate()` 函数）：

```c
/* ============================================================================
 * 游标比值累计（1kHz 成对采样）
 * ...
 * ============================================================================ */

static uint16_t s_ratio_prev_rotor;
static uint16_t s_ratio_prev_output;
static bool s_ratio_primed;
static int64_t s_ratio_sum_rotor;
static int64_t s_ratio_sum_output;
static uint32_t s_ratio_pairs;

static void ratio_accumulate(uint16_t rotor_raw, uint16_t output_raw) {
    ...整个函数体...
}
```

删除 `static int encoder_sample_output(void) { ... }` 整个函数（含其上方那条孤立的旧 doc 注释块 `@brief 读一次编码器 + 计时 + 更新观测变量`——该注释是历史残留，函数体属于 encoder_sample_output）。

删除统计区的 `static uint32_t s_sample_index;`。

- [ ] **Step 3: `app_debug_encoder_sample()` 收敛为转子单路**

将函数体替换为：

```c
void app_debug_encoder_sample(void) {
    uint16_t raw;
    bool valid;

    /* 转子：采样在 GPTMR 12.5kHz 采样 ISR（SPI3 独占）；此处只读一致快照
     * 刷新观测变量（零 SPI 成本）。出轴采样已按 M1 裁剪移除（spec §3）。 */
    if ((app_encoder_get_rotor_raw(&raw, &valid, NULL) == 0) && valid) {
        uint16_t zero = 0U;

        g_enc_rotor_raw = raw;
        if (app_encoder_get_zero(APP_ENCODER_ROTOR, &zero) == 0) {
            g_enc_rotor_deg = (float)(uint16_t)(raw - zero) * (360.0f / 65536.0f);
        }
    }
    s_read_cycles_sum[APP_ENCODER_ROTOR] += 0U;
    s_read_count[APP_ENCODER_ROTOR]++;
    s_loop_count++;
}
```

- [ ] **Step 4: `print_stats()` 剔除 vernier 块**

删除该函数中的这一整块（`/* 游标比值（1kHz 成对采样累计）… */` 起，到 `else { ... waiting for motion ... }` 止）：

```c
    /* 游标比值（1kHz 成对采样累计）：刚体传动下 ratio = ΔO/ΔR = 齿比 */
    if (s_ratio_sum_rotor != 0) {
        ...
    } else {
        app_debug_printf(
            "[ENC] vernier: waiting for motion (pairs=%u)\r\n", (unsigned)s_ratio_pairs);
    }
```

其余 print_stats 内容不动（`output` 相关字段读数恒 0，属预期——出轴未采样）。

- [ ] **Step 5: 验证手术完整**

```bash
grep -n 'encoder_sample_output\|ratio_accumulate\|s_ratio_\|s_sample_index\|ENC_OUTPUT_SAMPLE_DIV' App/Debug/Src/app_debug_encoder.c
grep -n 'note_loop_late\|app_debug_encoder_sample\b\|passive_selftest' App/Debug/Src/app_debug_encoder.c | head
git add App/Debug/Inc/app_debug_encoder.h App/Debug/Src/app_debug_encoder.c
```
Expected: 第一条**无输出**（出轴机器清干净）；第二条命中 `note_loop_late`、`app_debug_encoder_sample`、`passive_selftest`（保留项在位）。

---

## Task 6: AGENTS.md 并集 + drv_hrpwm.c 双修保留 + app_debug_motor.c 处置

**Files:**
- Resolve: `AGENTS.md`、`Driver/hpm_impl/drv_hrpwm.c`、`App/Debug/Src/app_debug_motor.c`

**裁决要点**（spec §3）：`drv_hrpwm.c` 取 rtos 侧的「CMP 写无 UNLK」+ **保留** foc-v1 的 `hrpwm_set_duty` 实例越界防御（两处改动位于不同函数，理想情况 git 已自动合并；人工核对两边都在）。`AGENTS.md` 两侧新增段落并集（rtos: FreeRTOS 引入纪律；foc-v1: 电流采样符号）。`app_debug_motor.c` 冲突随便解（`--ours`），文件保留在树中但不编译（Task 2 已剔除）。

- [ ] **Step 1: drv_hrpwm.c —— 确认/合并双修**

```bash
git status --short Driver/hpm_impl/drv_hrpwm.c
grep -n 'pwm_shadow_register_unlock' Driver/hpm_impl/drv_hrpwm.c
grep -n 'instance >=\|hrpwm_write_cmp_pair' Driver/hpm_impl/drv_hrpwm.c
```
Case A（自动合并，`git status` 显示 `M ` 已暂存）：直接进 Step 2 验证。
Case B（仍冲突 `UU`）：按下述两区域手工合并后 `git add`：
- `hrpwm_write_cmp_pair()`：采用 rtos 版（**不**含 `pwm_shadow_register_unlock`，含 \"直接写 CMP 工作寄存器…禁止 pwm_shadow_register_unlock\" doc 注释）。
- `hrpwm_set_duty()`：采用 foc-v1 版（含越界防御）：

```c
    map = hrpwm_get_channel_map(ch);
    /* instance 越界防御：Release(-O2) 下编译器无法证明映射表取值 < 实例数，
     * 显式校验（越界写会直接踩坏相邻状态） */
    if ((map == NULL) || (map->instance >= (uint8_t)(sizeof(s_hrpwm_instances)
                                                    / sizeof(s_hrpwm_instances[0])))
        || !hrpwm_is_valid_duty(duty)) {
        return -1;
    }
```

- [ ] **Step 2: 验证 drv_hrpwm 双修在位**

```bash
grep -c 'pwm_shadow_register_unlock' Driver/hpm_impl/drv_hrpwm.c
grep -c 'instance >=' Driver/hpm_impl/drv_hrpwm.c
git add Driver/hpm_impl/drv_hrpwm.c
```
Expected: 第一条的命中**仅出现在非 write_cmp_pair 函数**（init/触发配置路径的 UNLK 是合法的；write_cmp_pair 内不得有——用 `sed -n '/hrpwm_write_cmp_pair/,/^}/p' Driver/hpm_impl/drv_hrpwm.c | grep -c unlock` 应输出 `0`）；第二条 ≥1（越界防御在）。

- [ ] **Step 3: AGENTS.md 并集**

若 `git status` 显示 `UU AGENTS.md`：`git checkout --ours AGENTS.md` 后，把 rtos 侧 `### FreeRTOS 引入纪律（2026-09-23）` 整节（从该标题行到文件尾，内容以 \"**必须 `sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)`\" 开头、以 \"USB 行为零变化…\" 段落结束）追加到文件末尾。提取命令：

```bash
git show feat/rtos-foundation:AGENTS.md | sed -n '/^### FreeRTOS 引入纪律/,$p' >> AGENTS.md
git add AGENTS.md
```
若已自动合并（`M `）：验证两节都在即可：

```bash
grep -c 'FreeRTOS 引入纪律\|电流采样符号' AGENTS.md
```
Expected: ≥2（两节标题都在）。

- [ ] **Step 4: app_debug_motor.c 冲突处置（裁掉，保留源）**

```bash
git checkout --ours App/Debug/Src/app_debug_motor.c
git add App/Debug/Src/app_debug_motor.c
grep -c 'sdk_app_src.*app_debug_motor' CMakeLists.txt
```
Expected: 冲突标记消除；CMake 引用数 `0`（Task 2 已剔除；此 grep 是复核）。

---

## Task 7: identify 调用点空桩（app_foc.c + app_debug_foc.c）

**Files:**
- Modify: `App/Control/Src/app_foc.c`、`App/Debug/Src/app_debug_foc.c`

**裁决要点**（spec §4 \"identify 调用点→空桩\"）：`app_motor_identify.c`/`id_encoder.c` 不编译（Task 2 已剔除）→ 所有 `app_motor_identify_*` 调用点清空，含 spec 树只点名 `app_foc.c`、但同样引用 identify 的 `app_debug_foc.c`（spec 覆盖缺口，本任务补齐）。CALIB 状态机与 Ozone `cal_*` 字段保留（布局稳定，重新接入辨识时恢复）。

- [ ] **Step 1: app_foc.c —— 3 处**

删除 `#include "app_motor_identify.h"` 行。

将 run_body 中的辨识分支（约 L234-237）：

```c
    /* 辨识模式：由辨识模块提供激励（强制角 + 电流给定）；电流环在 ISR 内执行 */
    if (s_state == APP_FOC_STATE_CALIB) {
        (void)app_motor_identify_fast_step();
    }
```
替换为：

```c
    /* 辨识模式：M1 裁剪（spec §1），app_motor_identify 不参与构建。
     * CALIB 状态/接口保留；重新接入辨识时在此恢复 app_motor_identify_fast_step()。 */
```

将 disable() 尾部的（约 L598-605）：

```c
    /* 3) 中止电气标定编排（若进行中）：避免其状态在 FOC 已 OFF 后悬挂。
     *    此时 state 已是 OFF，exit_calib 不会被误触发回 READY。 */
    if (app_motor_identify_is_active()) {
        app_motor_identify_abort();
    }
```
替换为：

```c
    /* 3) 电气标定中止：M1 裁剪（辨识不构建），无中止对象。 */
```

- [ ] **Step 2: app_debug_foc.c —— 4 处**

删除 `#include "app_motor_identify.h"` 行。

dispatch 中（约 L60-64）：

```c
    case APP_DEBUG_FOC_CMD_CAL_ENCODER:
        return (int32_t)app_motor_identify_start();
    case APP_DEBUG_FOC_CMD_CAL_ABORT:
        app_motor_identify_abort();
        return 0;
```
替换为：

```c
    case APP_DEBUG_FOC_CMD_CAL_ENCODER:
    case APP_DEBUG_FOC_CMD_CAL_ABORT:
        return -1; /* M1 裁剪：辨识不参与构建 */
```

refresh 中（约 L83-88 的 idr 声明 + 取值，及约 L125-132 的 cal_* 赋值）：删除

```c
    app_motor_identify_result_t idr;
    ...
    app_motor_identify_get_result(&idr);
```
以及 8 行 `g_app_debug_foc.cal_* = idr....;` 赋值，替换为一行注释 + 显式清零（保持 Ozone 字段确定性）：

```c
    /* 辨识结果域：M1 裁剪（辨识不构建），恒 0（重新接入后由 get_result 回填） */
    g_app_debug_foc.cal_active = 0U;
    g_app_debug_foc.cal_done = 0U;
    g_app_debug_foc.cal_failed = 0U;
    g_app_debug_foc.cal_fail_reason = 0U;
    g_app_debug_foc.cal_progress = 0.0f;
    g_app_debug_foc.cal_offset_rad = 0.0f;
    g_app_debug_foc.cal_direction = 0.0f;
    g_app_debug_foc.cal_quality = 0.0f;
```

tick 中（约 L202-204）：

```c
    /* 3) 电气标定 1kHz 推进（集中于此，无 Terminal 依赖；台架模式同样生效） */
    if (app_motor_identify_is_active()) {
        app_motor_identify_run_once(g_app_debug_foc.tick_count);
    }
```
替换为：

```c
    /* 3) 电气标定推进：M1 裁剪（辨识不构建）。 */
```

- [ ] **Step 3: 验证 identify 引用清零**

```bash
grep -rn 'app_motor_identify_\|id_encoder_' App/ --include='*.c' | grep -v 'app_motor_identify\.\|id_encoder\.'
grep -rn 'app_motor_identify' App/Control/Src/app_foc.c App/Debug/Src/app_debug_foc.c
```
Expected: 第一条**仅剩注释行**（M1 裁剪说明中的函数名提及）与头文件自述；第二条**无输出或仅注释**。硬标准：`grep -n 'app_motor_identify_\(start\|abort\|fast_step\|run_once\|is_active\|get_result\)' App/Control/Src/app_foc.c App/Debug/Src/app_debug_foc.c` **无输出**（无实际调用）。

```bash
git add App/Control/Src/app_foc.c App/Debug/Src/app_debug_foc.c
```

---

## Task 8: V/F 调用点清理 + app_terminal_cmd_motor.c 重写

**Files:**
- Modify: `App/Debug/Src/app_debug_cmd.c`、`App/Comm/terminal/Src/app_terminal_cmd.c`
- Rewrite: `App/Comm/terminal/Src/app_terminal_cmd_motor.c`

**裁决要点**（Q2a + spec §4）：V/F 命令入口移除（`motor start/stop/freq/mod`、`cal encoder`、`inv`）；保留 `motor iq` + `cal current`。`app_debug_cmd.c` 的单字符 `1/2/3/a/0`（逐相故障定位）保留，但其安全前置 `app_debug_motor_stop()` 改 `app_foc_disable()`（V/F 已裁，唯一被驱动源是 FOC）。`app_terminal_cmd.c` 的 `require_motor_stopped` 依赖 `app_debug_motor_is_running` → 改为「FOC 未活动」语义。

- [ ] **Step 1: app_debug_cmd.c —— 3 处**

`#include "app_debug_motor.h"` 替换为 `#include "app_foc.h"`。

5 处 `app_debug_motor_stop();` 替换为 `app_foc_disable();`（cases `1`/`2`/`3`/`a`/`0` 内；`0` 处的注释 `/* 安全：先停旋转（含归零矢量） */` 改为 `/* 安全：先停 FOC（含归零矢量） */`）。

删除整个 V/F 单字符块：

```c
        /* 开环旋转自检（V/F） */
        case 'r': app_debug_motor_rotation_toggle(); break;
        case '+': app_debug_motor_freq_step(1); break;
        case '-': app_debug_motor_freq_step(-1); break;
        case 'm': app_debug_motor_mod_step(-1); break;
        case 'M': app_debug_motor_mod_step(1); break;
```

文件头注释中删除 `r = 开环旋转启停（V/F）；+/- = 电频率 ±0.5Hz；m/M = 调制比 ∓/±1%` 一行。

- [ ] **Step 2: app_terminal_cmd.c —— require_motor_stopped 语义修正**

`#include "app_debug_motor.h"` 替换为 `#include "app_foc.h"`。

```c
bool app_terminal_cmd_require_motor_stopped(chry_shell_t* csh) {
    if (app_debug_motor_is_running()) {
        csh_printf(csh, "ERR: motor is running, stop it first (motor stop)\r\n");
        return false;
    }
    return true;
}
```
替换为：

```c
bool app_terminal_cmd_require_motor_stopped(chry_shell_t* csh) {
    /* V/F 开环已裁剪（M1）：唯一被驱动源为 FOC */
    if (app_foc_is_active()) {
        csh_printf(csh, "ERR: FOC is active (use 'foc off' first)\r\n");
        return false;
    }
    return true;
}
```

- [ ] **Step 3: app_terminal_cmd_motor.c 整文件替换（仅 motor iq + cal current）**

```c
/**
 * @file    app_terminal_cmd_motor.c
 * @brief   Terminal 电机命令（motor / cal）—— M1：仅 FOC 转矩给定与电流零点标定
 * @author  Kaiser
 *
 * 命令：
 *   motor iq [<A>]     （FOC 转矩给定；无参 = 查询）
 *   cal current        （电流零点标定）
 *
 * M1 裁剪（spec §1/§4）：V/F 开环（motor start/stop/freq/mod）、逆变桥逐相
 * （inv）、电角度辨识（cal encoder）不接入；相关源文件不参与构建。重新接入时
 * 恢复本文件相应分支，并把 app_debug_motor.c / app_motor_identify.c 加回
 * CMakeLists.txt。
 *
 * 安全联锁：驱动类命令要求无故障锁存；标定要求 FOC 关闭。
 * 命令本体快速返回，不阻塞控制环。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_cmd.h"

#include "app_terminal.h"
#include "app_terminal_job.h"

#include "app_analog_signal.h"
#include "app_foc.h"

#include <string.h>

/**
 * @brief 命令 motor：iq（FOC 转矩给定）。
 *
 * 用法：
 *   motor [iq [<A>]]   无参 = 查询；有参 = 设置（限幅 ±i_q_max）
 */
static int cmd_motor(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);
    const char* sub = (argc >= 2) ? argv[1] : "iq";

    if ((strcmp(sub, "help") == 0) || (strcmp(sub, "status") == 0)) {
        csh_printf(csh, "usage: motor iq [<A>]   (FOC torque ref; no arg = query)\r\n");
        return 0;
    }

    if (strcmp(sub, "iq") == 0) {
        float value;
        app_foc_current_snapshot_t snap;

        if (app_foc_get_state() == APP_FOC_STATE_CALIB) {
            csh_printf(csh, "ERR: FOC busy (calibration in progress)\r\n");
            return -1;
        }
        if ((app_foc_get_state() != APP_FOC_STATE_READY)
            && (app_foc_get_state() != APP_FOC_STATE_RUN)) {
            csh_printf(csh, "ERR: FOC not enabled (use 'foc on')\r\n");
            return -1;
        }
        if (argc >= 3) {
            if (app_terminal_cmd_parse_float(argv[2], &value) != 0) {
                csh_printf(csh, "ERR: invalid value '%s'\r\n", argv[2]);
                return -1;
            }
            if (app_foc_set_iq_ref(value) != 0) {
                csh_printf(csh, "ERR: set iq failed\r\n");
                return -1;
            }
        }
        /* 回显实际生效给定（getter）与最新测量；快照为上一拍数据 */
        app_foc_get_snapshot(&snap);
        csh_printf(csh, "foc iq: ref=%.3f A  avg=%.3f A  now=%.3f A\r\n",
                   (double)app_foc_get_iq_ref(), (double)snap.i_q_avg_a, (double)snap.i_q_a);
        return 0;
    }

    csh_printf(csh, "ERR: unknown subcommand '%s'\r\n", sub);
    return app_terminal_cmd_usage(csh, "motor iq [<A>]");
}

/**
 * @brief cal current job：1kHz 驱动非阻塞标定状态机。
 * @param now_ms 系统毫秒计数（未用）
 */
static void cal_tick(uint32_t now_ms) {
    int rc;

    (void)now_ms;

    rc = app_analog_signal_calibrate_step();
    if (rc == 1) {
        return; /* 进行中 */
    }

    if (rc == 0) {
        app_terminal_cmd_emit("\r\nOK: ADC zero calibration\r\n");
    } else {
        app_terminal_cmd_emit("\r\nFAIL: ADC zero calibration (no current required)\r\n");
    }
    app_terminal_job_abort(); /* 完成/失败：结束 job（触发 abort 回调换行+刷新） */
}

/**
 * @brief cal current job 中止回调（用户取消或完成）。
 */
static void cal_abort(void) {
    app_analog_signal_calibrate_cancel();
    app_terminal_cmd_emit("\r\n");
    app_terminal_refresh();
}

/** cal current job（静态生命周期；active 由框架维护） */
static app_terminal_job_t s_cal_job = {
    .name = "cal current",
    .tick = cal_tick,
    .abort = cal_abort,
    .active = false,
};

/**
 * @brief 命令 cal：电流零点标定（current），job 驱动，不阻塞控制环。
 */
static int cmd_cal(int argc, char** argv) {
    chry_shell_t* csh = app_terminal_cmd_ctx(argc, argv);

    if ((argc < 2) || (strcmp(argv[1], "current") != 0)) {
        return app_terminal_cmd_usage(csh, "cal current");
    }
    if (!app_terminal_cmd_require_motor_stopped(csh)) {
        return -1;
    }
    if (!app_terminal_cmd_require_no_fault(csh)) {
        return -1;
    }
    if (app_foc_get_state() != APP_FOC_STATE_OFF) {
        csh_printf(csh, "ERR: FOC enabled (use 'foc off' first)\r\n");
        return -1;
    }

    /* 先启动 job（内部会中止旧 job → 旧 cal 的 abort 回调取消旧标定），再启动新标定 */
    if (app_terminal_job_start(&s_cal_job) != 0) {
        csh_printf(csh, "FAIL: job start\r\n");
        return -1;
    }
    if (app_analog_signal_calibrate_start() != 0) {
        app_terminal_job_abort();
        csh_printf(csh, "FAIL: calibration start\r\n");
        return -1;
    }

    csh_printf(csh, "calibrating current zero (256 frames, any key to cancel)...\r\n");
    return 0;
}

CSH_CMD_EXPORT_ALIAS(cmd_motor, motor, );
CSH_CMD_EXPORT_ALIAS(cmd_cal, cal, );
```

- [ ] **Step 4: 验证 V/F 与 identify 命令入口清零**

```bash
grep -rn 'app_debug_motor_' App/ --include='*.c' --include='*.h'
grep -n 'cal encoder\|motor start\|cmd_inv\|rotation_toggle' App/Comm/terminal/Src/app_terminal_cmd_motor.c App/Debug/Src/app_debug_cmd.c
```
Expected: 第一条**无输出**（`app_debug_motor_*` 全树零调用）；第二条**无输出**。另核对导出：`grep -c 'CSH_CMD_EXPORT_ALIAS(cmd_inv' App/Comm/terminal/Src/app_terminal_cmd_motor.c` = `0`。

```bash
git add App/Debug/Src/app_debug_cmd.c App/Comm/terminal/Src/app_terminal_cmd.c App/Comm/terminal/Src/app_terminal_cmd_motor.c
```

---

## Task 9: 全量构建 + 回归测试 + 符号核查 → 合并提交

**Files:** 无新增修改（验证 + 一次性 merge commit）；若构建报错，按「最小修复」就地改并重新验证。

- [ ] **Step 1: 清理构建 + 配置 + 编译**

```bash
make clean && make configure && make build
tail -5 build/last_build.log
```
Expected: 构建成功（`last_build.log` 末尾无 error）。失败时：只看 `last_build.log` 第一个真实错误（连锁报错不处理），最小修复后重跑。已知风险点：遗漏的 `app_motor_identify_*`/`app_debug_motor_*` 残留引用（Task 7/8 验证可兜住）；`app_debug_encoder.c` 手术后未删净静态量（-Werror 会拦）。

- [ ] **Step 2: 导出产物 + 符号核查**

```bash
make artifacts
export PATH=/opt/riscv32-gnu-toolchain-elf-bin/bin:$PATH
riscv32-unknown-elf-nm output/HPM53M1_G6618Motor.elf | grep -E ' (T|t) (freertos_exception_handler|app_foc_isr_step|app_motor_identify_start|app_debug_motor_run_once|algo_trig_sin_cos|app_fast_step_hook)$'
```
Expected: `freertos_exception_handler`（强符号，来自 app_rtos.c）、`app_foc_isr_step` 在；**无** `app_motor_identify_start`、**无** `app_debug_motor_run_once`、**无** `algo_trig_sin_cos`（裁剪生效）。`app_fast_step_hook` 为 static 可能不在符号表——若无则改用 `riscv32-unknown-elf-nm output/HPM53M1_G6618Motor.elf | grep fast_step` 放宽匹配。

补充核对非向量模式接线（防 PLIC 地址 bug 回潮）：

```bash
riscv32-unknown-elf-nm output/HPM53M1_G6618Motor.elf | grep -c 'freertos_handle_interrupt'
```
Expected: ≥1（非向量模式 ISR 路径在位）。

- [ ] **Step 3: 主机单测回归（算法/控制/编码器零回归）**

```bash
bash scripts/tests/foc/run.sh 2>&1 | tail -3
bash scripts/tests/control/run.sh 2>&1 | tail -3
bash scripts/tests/encoder/run.sh 2>&1 | tail -3
```
Expected: 三脚本 PASS（与 Task 1 基线一致）。这些测试直接编 `App/Algorithm/FOC/Src/*.c` 与 `app_foc_current.c` 等（不经过固件 CMake），所以 `id_encoder.c` 不进固件不影响其主机测试。

- [ ] **Step 4: 合并提交（一次性）**

```bash
git add -A
git status --short | grep -E '^(UU|AA|DU|UD)' && echo 'STILL CONFLICTED' || echo 'ALL RESOLVED'
git commit --no-edit
git log --oneline -3
git rev-parse HEAD
```
Expected: `ALL RESOLVED`；生成 merge commit（消息含两分支名），记录其 SHA 为 `<MERGE_SHA>`。提交说明若需人工润色用 `git commit` 打开编辑器，要点：\"合并 feat/rtos-foundation：FreeRTOS 框架 × FOC 电流环 M1（辨识/V-F 编译级裁剪，保护动作全关）\"。

回滚命令（备用）：`git revert -m 1 <MERGE_SHA>` 或 `git reset --hard <ROLLBACK_B>`。

---

## Task 10: app_fault.c OV/UV 改「跟随 shutdown_en」→ 独立提交

**Files:**
- Modify: `App/Control/Src/app_fault.c`

**裁决要点**（spec §6，Kaiser 裁决）：OV/UV 只判断提醒、不做实际动作（M1），待电流内环跑通后逐步加回动作。实现 = 取消 OV/UV 的「无条件停机」硬例外，与其它故障同样跟随 `fault.shutdown_en`。检测/计数/锁存/告警全保留。这是**保护逻辑唯一改动**，独立成 commit（单独 revert 即把 OV/UV 停机动作加回，作为旋转测试前的动作加回路径）。

- [ ] **Step 1: 修改状态汇总块**

在 `app_fault_tick()` 的状态汇总块中（注释 \"---- 状态汇总（FAULT 锁存优先） ----\" 起）：

```c
    /* ---- 状态汇总（FAULT 锁存优先） ----
     * software.fault.shutdown_en = 0（台架模式）：过流/健康类保护仍检测、计数、
     * 上报，但状态不进入 FAULT（消费方据此停机），仅保持 WARNING。
     * 例外：母线过压/欠压仍保持停机 —— 反灌抬压是真实硬件风险（电容耐压），
     * 限流电源无法吸收回馈能量，此保护不可关。 */
    {
        uint32_t hard = s_fault_ctx.latched & (APP_FAULT_VBUS_OV | APP_FAULT_VBUS_UV);
        uint32_t soft = s_fault_ctx.latched & ~(APP_FAULT_VBUS_OV | APP_FAULT_VBUS_UV);
        bool shutdown_en = (app_software_params_current()->fault.shutdown_en != 0U);

        if ((hard != 0U) || (shutdown_en && (soft != 0U))) {
            s_fault_ctx.state = APP_FAULT_STATE_FAULT;
        } else if ((soft != 0U) || pending) {
```

替换为：

```c
    /* ---- 状态汇总（FAULT 锁存优先） ----
     * software.fault.shutdown_en = 0（台架模式）：全部保护仅检测、计数、上报，
     * 状态不进入 FAULT（消费方据此停机），仅保持 WARNING。
     * M1 修订（spec §6）：母线 OV/UV 同样只判断提醒、不做停机动作 ——
     * 取消「OV/UV 无条件停机」例外，跟随 shutdown_en。检测/告警保留不变。
     * 注意：反灌抬压风险仍在（限流电源不吸收回馈能量），进入旋转测试前
     * 须逐步加回 OV/UV 停机动作（revert 本提交即恢复硬停机路径）。 */
    {
        uint32_t soft = s_fault_ctx.latched;
        bool shutdown_en = (app_software_params_current()->fault.shutdown_en != 0U);

        if (shutdown_en && (soft != 0U)) {
            s_fault_ctx.state = APP_FAULT_STATE_FAULT;
        } else if ((soft != 0U) || pending) {
```

- [ ] **Step 2: 验证 + 重新构建**

```bash
sed -n '/状态汇总/,/fault_publish/p' App/Control/Src/app_fault.c | grep -c 'hard'
make build && make artifacts
```
Expected: 第一条 `0`（hard/soft 二分已消除）；构建成功。

- [ ] **Step 3: 独立提交**

```bash
git add App/Control/Src/app_fault.c
git commit -m "fix(fault): OV/UV 改跟随 shutdown_en（M1 只判断提醒不动作）"
git log --oneline -3
```
Expected: `<MERGE_SHA>` 之上一个新 commit；回滚 = `git revert <此commit>` 恢复 OV/UV 硬停机。

---

## Task 11: 交付报告（合并后代码结构 + 测试指导）

**Files:** 无代码修改（输出交付）。

- [ ] **Step 1: 回报合并后代码结构**

向 Kaiser 输出最终目录树（Task 2/3/4/5 的裁决结果落地后即为 spec §4 的树，核对后照实回报，含 [来源] 标注：foc-v1 / rtos / 重写 / 不编译）：

```bash
git ls-tree -r --name-only HEAD | grep -E '^(App|Interface|Driver|Board|config)/' | sort
```
与 spec §4 树逐一核对；不一致处如实列出。报告要点：
- 快车道：`current_hook = app_fast_step_hook`（mcycle 计时）→ `app_foc_isr_step`（foc-v1 原样：编码器快照 → foc_angle → clarke/park → PI → inv_park → foc_modulation → set_duty_abc）。
- 任务模型：io(1ms)/diag(1s)/rtt_log(prio4)；非向量+非抢占；`freertos_exception_handler` 强符号指纹在位。
- 不编译清单：`app_motor_identify.c`、`id_encoder.c`、`app_debug_motor.c`、`algo_trig.c`（源保留，CMake 剔除，一行可复编）。
- 保护：动作全关（`foc bench 1` → `shutdown_en=0`）；OV/UV 只告警（Task 10）；ISR 超预算停机保留。

- [ ] **Step 2: 回报 M1 测试指导（引用 spec §7，含实测命令）**

给 Kaiser 的上板序列（摘自 spec §7.2，命令可照抄）：

1. PSU：**24V**（台架母线），限流 **2A** 起（覆盖 `foc vtest 0.10` → 预期 0.63A），加大给定按最大电流 ×2 抬；上电顺序 = 限流先于电压。
2. `cal current` —— ADC 零点（256 帧 ≈ 10ms，FOC 必须 OFF）。
3. `foc bench 1` —— 关自动停机保护；确认打印 `i_trip=0 / speed_max=0 / shutdown_en=0`。
4. `foc on` → state=READY。
5. `foc vtest 0.10` —— 1.5s 自停；预期 `|i| ≈ 0.63A`（±20%），验电流符号/映射（历史坑 `9ba8ce3`）。
6. `foc vtest 0.2 90` —— 电流矢量方向随角度变（角度通路活着）。
7. `foc trace` → `motor iq 1.0` —— 5ms @25kHz dq 阶跃波形。
8. `motor iq 2.0 / 0.5 / -1.0` —— 回显 ref/avg/now 跟随；看 `v_scale`（稳态=1.0）。
9. `foc off` 收工。

验收判据（spec §7.3）：vtest 幅值 ±20%、符号正确；iq 阶跃无持续振荡、沉降 ≤2ms；`[ISR]` 单拍 <40µs 且 `headroom>0`；全程无意外停机（告警码允许，逐条记录）。失败排查 6 条按 spec §7.4 概率序。

- [ ] **Step 3: 提醒两条边界（真话）**

- 动作全关后硬保护只剩硬件死区 50ns + ISR 超预算停机；PSU 限流兜 ms 级失控，µs 级直通由死区兜底（spec §6）。
- OV/UV 不停机 → 本测试谱（静止）反灌能量极小，但**进入旋转测试前必须加回 OV/UV 停机动作**（revert Task 10 提交）。
