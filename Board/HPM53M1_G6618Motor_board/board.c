/**
 * @file    board.c
 * @brief   HPM53M1_G6618Motor_board 板级初始化
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"

#include "hpm_l1c_drv.h"

/** L1C 控制寄存器回读（.noncacheable.bss：Ozone/终端可查；bit9=IC_EN, bit8=DC_EN） */
volatile uint32_t g_board_l1c_ctl __attribute__((section(".noncacheable.bss")));
#include "pinmux.h"
#include "hpm_clock_drv.h"
#include "hpm_usb_drv.h"

/**
 * @brief FLASH configuration option definitions:
 * option[0]:
 *    [31:16] 0xfcf9 - FLASH configuration option tag
 *    [15:4]  0 - Reserved
 *    [3:0]   option words (exclude option[0])
 * option[1]:
 *    [31:28] Flash probe type
 *      0 - SFDP SDR / 1 - SFDP DDR
 *      2 - 1-4-4 Read (0xEB, 24-bit address) / 3 - 1-2-2 Read(0xBB, 24-bit address)
 *      4 - HyperFLASH 1.8V / 5 - HyperFLASH 3V
 *      6 - OctaBus DDR (SPI -> OPI DDR)
 *      8 - Xccela DDR (SPI -> OPI DDR)
 *      10 - EcoXiP DDR (SPI -> OPI DDR)
 *    [27:24] Command Pads after Power-on Reset
 *      0 - SPI / 1 - DPI / 2 - QPI / 3 - OPI
 *    [23:20] Command Pads after Configuring FLASH
 *      0 - SPI / 1 - DPI / 2 - QPI / 3 - OPI
 *    [19:16] Quad Enable Sequence (for the device support SFDP 1.0 only)
 *      0 - Not needed
 *      1 - QE bit is at bit 6 in Status Register 1
 *      2 - QE bit is at bit1 in Status Register 2
 *      3 - QE bit is at bit7 in Status Register 2
 *      4 - QE bit is at bit1 in Status Register 2 and should be programmed by 0x31
 *    [15:8] Dummy cycles
 *      0 - Auto-probed / detected / default value
 *      Others - User specified value, for DDR read, the dummy cycles should be 2 * cycles on FLASH datasheet
 *    [7:4] Misc.
 *      0 - Not used
 *      1 - SPI mode
 *      2 - Internal loopback
 *      3 - External DQS
 *    [3:0] Frequency option
 *      1 - 30MHz / 2 - 50MHz / 3 - 66MHz / 4 - 80MHz / 5 - 100MHz / 6 - 120MHz / 7 - 133MHz / 8 - 166MHz
 *
 * option[2] (Effective only if the bit[3:0] in option[0] > 1)
 *    [31:20]  Reserved
 *    [19:16] IO voltage
 *      0 - 3V / 1 - 1.8V
 *    [15:12] Pin group
 *      0 - 1st group / 1 - 2nd group
 *    [11:8] Connection selection
 *      0 - CA_CS0 / 1 - CB_CS0 / 2 - CA_CS0 + CB_CS0 (Two FLASH connected to CA and CB respectively)
 *    [7:0] Drive Strength
 *      0 - Default value
 * option[3] (Effective only if the bit[3:0] in option[0] > 2, required only for the QSPI NOR FLASH that not supports
 *              JESD216)
 *    [31:16] reserved
 *    [15:12] Sector Erase Command Option, not required here
 *    [11:8]  Sector Size Option, not required here
 *    [7:0] Flash Size Option
 *      0 - 4MB / 1 - 8MB / 2 - 16MB
 */
#if defined(FLASH_XIP) && FLASH_XIP
__attribute__ ((section(".nor_cfg_option"), used)) const uint32_t option[4] = {0xfcf90002, 0x00000005, 0x1000, 0x0};
#endif

#if defined(FLASH_UF2) && FLASH_UF2
ATTR_PLACE_AT(".uf2_signature") __attribute__((used)) const uint32_t uf2_signature = BOARD_UF2_SIGNATURE;
#endif

/**
 * @brief 关闭 USB PHY DP/DM 内部下拉（临时使能 USB0 时钟后恢复）
 */
static void board_disable_usb_phy_dp_dm_pulldown(void)
{
    if (!clock_check_in_group(clock_usb0, 0)) {
        clock_add_to_group(clock_usb0, 0);
    }
    usb_phy_disable_dp_dm_pulldown(HPM_USB0);
    clock_remove_from_group(clock_usb0, 0);
    while (sysctl_resource_target_is_busy(HPM_SYSCTL, sysctl_resource_usb0)) {
        ;
    }
}

/**
 * @brief 板级初始化（关闭 USB PHY 下拉 + 配置引脚复用）
 */
void board_init(void)
{
    /* L1 Cache（HPM53M1：16KB I + 16KB D，32B line）——必须尽早使能：
     * 不使能时所有 flash(XIP) 代码/常量按无 cache 速度取指，热路径慢 5~10 倍
     * （台架实测 FOC 单拍 100us、主循环 200us ≈ 5kHz，设计目标 25kHz）。
     * 安全性：本工程 DMA 缓冲全部位于 DLM 非缓存区
     * （linker 将 .noncacheable 与 .fast_ram 均放入 DLM 0x00080300）；
     * D-Cache 采用 write-around（写直达内存，不分配行），DMA 可见性不受影响。 */
    l1c_ic_enable();
    l1c_dc_enable();
    g_board_l1c_ctl = l1c_get_control(); /* 运行态回读：验证使能是否真正生效 */

    board_disable_usb_phy_dp_dm_pulldown();
    init_pins();
}

/**
 * @brief USB0 板级初始化（时钟就绪后调用）
 *
 *   - HPM53M1 的 USB_DP/USB_DM 为专用引脚（封装 pin48/49），无 IOMUX 配置项
 *   - QFN80 无 USB0_VBUS 检测引脚（原理图亦未引出），PHY 使用内部 VBUS
 *   - DP/DM 45Ω 下拉已在 board_init() 关闭，此处时钟就绪后再确认一次
 */
void board_init_usb(void)
{
    clock_add_to_group(clock_usb0, 0);
    usb_phy_disable_dp_dm_pulldown(HPM_USB0);
    usb_phy_using_internal_vbus(HPM_USB0);
}

/**
 * @brief 从核初始化（HPM53M1 单核，空实现）
 */
void board_init_core1(void)
{
}

