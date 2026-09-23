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
#   - DISABLE_IRQ_PREEMPTIVE=1：与 55/57 官方示例一致（非向量 + 非抢占组合
#     经官方验证；非向量 + 抢占无官方先例）。
# ============================================================================

sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)
sdk_compile_definitions(-DDISABLE_IRQ_PREEMPTIVE=1)

# 数学库（sinf/cosf 等，开环旋转与后续 FOC 使用）
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
# csh.h 无条件 include "csh_config.h" → 由 config/csh_config.h 提供。
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
# 中间件（CherryUSB）经 usb_config.h 使用 RTT 日志，需全局可见
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
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Platform/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Control/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Comm/terminal/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Comm/can/Inc)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/App/Debug/Inc)

sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/Driver/hpm_impl)
sdk_app_inc(${CMAKE_CURRENT_SOURCE_DIR}/Driver/IrqProfiler)

# Optional shared code, provided by the environment (HPMDEV_SHARED_DIR).
# Degrades gracefully when the project is built outside the workspace.
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
# ============================================================================

sdk_app_src(App/Algorithm/Src/algo_ffd.c)
sdk_app_src(App/Algorithm/Src/algo_filter.c)
sdk_app_src(App/Algorithm/Src/algo_hyst.c)
sdk_app_src(App/Algorithm/Src/algo_pid.c)
sdk_app_src(App/Algorithm/Src/algo_pll.c)
sdk_app_src(App/Algorithm/Src/algo_ramp.c)
sdk_app_src(App/Algorithm/Src/algo_rms.c)

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
# ============================================================================

sdk_app_src(App/Control/Src/app_fault.c)
sdk_app_src(App/Control/Src/app_motor_params.c)

# ============================================================================
# Comm Layer（通讯：终端 / CAN 协议）
#   terminal/ — USB CDC 终端（CherrySH 绑定 + 命令 + 常驻状态区）
#   can/      — CAN 协议（预留：电机控制报文 / 反馈报文，独立 spec）
# ============================================================================

sdk_app_src(App/Comm/terminal/Src/app_terminal.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_job.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_sys.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_diag.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_motor.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_cmd_param.c)
sdk_app_src(App/Comm/terminal/Src/app_terminal_monitor.c)

# ============================================================================
# Debug Layer
# ============================================================================

sdk_app_src(App/Debug/Src/app_debug_uart.c)
sdk_app_src(App/Debug/Src/app_debug_encoder.c)
sdk_app_src(App/Debug/Src/app_debug_cmd.c)
sdk_app_src(App/Debug/Src/app_debug_flash.c)
sdk_app_src(App/Debug/Src/app_debug_inverter.c)
sdk_app_src(App/Debug/Src/app_debug_motor.c)
sdk_app_src(App/Debug/Src/app_debug_can.c)
sdk_app_src(App/Debug/Src/app_debug_adc.c)
sdk_app_src(App/Debug/Src/app_debug_fault.c)
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
# Generate IDE Projects
# ============================================================================

generate_ide_projects()
