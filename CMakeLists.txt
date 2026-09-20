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

# Linker scripts
set(CUSTOM_GCC_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/gcc/user_linker.ld)
set(CUSTOM_SES_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/segger/user_linker.icf)
set(CUSTOM_IAR_LINKER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/linkers/iar/user_linker.icf)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

project(user_app)

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
sdk_app_src(App/Platform/Src/app_hw_params.c)
sdk_app_src(App/Platform/Src/app_sw_params.c)

# ============================================================================
# Control Layer
# ============================================================================

sdk_app_src(App/Control/Src/app_fault.c)
sdk_app_src(App/Control/Src/app_motor_params.c)

# ============================================================================
# Debug Layer
# ============================================================================

sdk_app_src(App/Debug/Src/app_debug_uart.c)
sdk_app_src(App/Debug/Src/app_debug_usb.c)
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
sdk_app_src(App/main.c)

# ============================================================================
# Generate IDE Projects
# ============================================================================

generate_ide_projects()
