# G6618 DFU Bootloader（TinyUSB DFU 1.1 + MS OS 2.0 / WCID）
#
# 独立构建目录：build-bootloader/
#   make bootloader
#
# 与主工程的关系：
#   - 复用 Board/<board>/ 的 board.c（引脚/时钟前提）与 Driver/hpm_impl/drv_clock.c；
#   - 复用 Interface/intf_boot.h + App/Platform/Src/boot_meta.c（元数据/邮箱契约）；
#   - 链接脚本 linkers/gcc/bootloader.ld：FLASH 限制在 0x80003000 - 0x8001D000。
#
# TinyUSB 采用“手工引入源文件”方式（同 CherrySH/RTT），不启用 CONFIG_TINYUSB：
#   SDK 的 middleware/tinyusb/CMakeLists.txt 会强制编入 hw/bsp/board.c 与
#   hw/bsp/hpmicro/family.c，其中 board_millis() 使用 rdcycle(0xC00)；
#   本工程统一使用 mcycle(0xB00)，且自行提供 USB ISR 与时基。
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 提供 components/usb/device（hpm_usb_device.c，TinyUSB HPM dcd 依赖）
set(CONFIG_USB_DEVICE 1)

set(BOOTLOADER_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(PROJECT_ROOT ${BOOTLOADER_ROOT}/..)

set(CUSTOM_GCC_LINKER_FILE ${PROJECT_ROOT}/linkers/gcc/bootloader.ld)
set(CUSTOM_SES_LINKER_FILE ${PROJECT_ROOT}/linkers/segger/user_linker.icf)
set(CUSTOM_IAR_LINKER_FILE ${PROJECT_ROOT}/linkers/iar/user_linker.icf)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

project(g6618_bootloader)

# 非向量中断模式（与主工程一致）：
#   向量模式的 COMPLETE_IRQ_HANDLING_M 写死 0xE4200004（PLIC context0 complete），
#   且 start.S 会打开 PLIC 向量模式；本工程统一走 trap.c 的 claim/dispatch/complete
#   路径（intc_m_claim_irq / intc_complete_irq 使用 HPM_PLIC_BASE 宏）。
sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)

# ============================================================================
# TinyUSB（手工源文件列表）
# ============================================================================
set(TINYUSB_DIR $ENV{HPM_SDK_BASE}/middleware/tinyusb/src)

sdk_compile_definitions(-DCFG_TUSB_MCU=OPT_MCU_HPM)

sdk_app_inc(${BOOTLOADER_ROOT})
sdk_app_inc(${TINYUSB_DIR})
sdk_app_inc(${PROJECT_ROOT}/Interface)
sdk_app_inc(${PROJECT_ROOT}/App/Platform/Inc)
sdk_app_inc(${PROJECT_ROOT}/Driver/hpm_impl)

sdk_app_src(${TINYUSB_DIR}/tusb.c)
sdk_app_src(${TINYUSB_DIR}/common/tusb_fifo.c)
sdk_app_src(${TINYUSB_DIR}/device/usbd.c)
sdk_app_src(${TINYUSB_DIR}/device/usbd_control.c)
sdk_app_src(${TINYUSB_DIR}/portable/hpm/dcd_hpm.c)
sdk_app_src(${TINYUSB_DIR}/class/dfu/dfu_device.c)

# ============================================================================
# Bootloader
# ============================================================================
sdk_app_src(
    src/main.c
    src/boot.c
    src/flash.c
    src/dfu.c
    src/dfu_desc.c
)
sdk_app_src(${PROJECT_ROOT}/App/Platform/Src/boot_meta.c)
sdk_app_src(${PROJECT_ROOT}/App/Platform/Src/boot_mailbox.c)
sdk_app_src(${PROJECT_ROOT}/Driver/hpm_impl/drv_clock.c)

sdk_link_libraries(m)

generate_ide_projects()
