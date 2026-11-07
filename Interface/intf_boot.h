/**
 * @file    intf_boot.h
 * @brief   Bootloader / 应用共享的升级契约（分区、镜像头、元数据日志、邮箱）
 * @author  Kaiser
 *
 * 布局（HPM53M1，1MB NOR，A/B 双槽）：
 *   0x80000000 - 0x80003000   12KB  nor cfg option + ROM boot header
 *   0x80003000 - 0x8001D000  104KB  bootloader（TinyUSB DFU）
 *   0x8001D000 - 0x8001E000    4KB  Slot A 镜像日志（追加写）
 *   0x8001E000 - 0x8001F000    4KB  Slot B 镜像日志（追加写）
 *   0x8001F000 - 0x80020000    4KB  启动控制日志（追加写）
 *   0x80020000 - 0x8008E000  440KB  Slot A
 *   0x8008E000 - 0x8008F000    4KB  保留（槽间对齐间隙）
 *   0x8008F000 - 0x800FD000  440KB  Slot B
 *   0x800FD000 - 0x800FFFFF   12KB  保留区（齿槽/参数/自检，两槽共用）
 *
 * 镜像头 64B 位于槽基址，入口固定在 槽基址 + 0x40。
 * 元数据采用“追加写 + 记录 CRC”，断电最多丢失最后一条未完成记录。
 *
 * 邮箱（SYSCTL CPU0 GPR，软复位后保持）：
 *   GPR[12] = BOOT_MAILBOX_MAGIC, GPR[13] = 请求码。
 *   应用写 "G661" + "DFU0" 后软复位，bootloader 消费后进 DFU。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef INTF_BOOT_H
#define INTF_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 分区常量
 * ============================================================================ */
#define BOOT_FLASH_BASE           (0x80000000UL)
#define BOOT_FLASH_SIZE           (0x00100000UL)
#define BOOT_BOOTLOADER_END       (0x8001D000UL) /* bootloader 链接上限（不含） */
#define BOOT_META_A_ADDR          (0x8001D000UL)
#define BOOT_META_B_ADDR          (0x8001E000UL)
#define BOOT_CONTROL_ADDR         (0x8001F000UL)
#define BOOT_SLOT_A_ADDR          (0x80020000UL)
#define BOOT_SLOT_B_ADDR          (0x8008F000UL)
#define BOOT_SLOT_SIZE            (0x0006E000UL) /* 440KB，4KB 对齐 */
#define BOOT_SLOT_GAP_ADDR        (0x8008E000UL) /* 两槽之间的 4KB 对齐间隙（保留） */
#define BOOT_META_SECTOR_SIZE     (0x00001000UL)
#define BOOT_SLOT_COUNT           (2U)

/* ============================================================================
 * 镜像头（64B，槽基址）
 * ============================================================================ */
#define BOOT_IMAGE_SIGNATURE      (0x0A4D5048UL) /* 沿用 BOARD_UF2_SIGNATURE */
#define BOOT_IMAGE_MAGIC          (0x47494D47UL) /* "GIMG" */
#define BOOT_IMAGE_HEADER_VERSION (1U)
#define BOOT_IMAGE_HEADER_SIZE    (0x40U)
#define BOOT_IMAGE_ENTRY_OFFSET   (0x40U)
#define BOOT_IMAGE_HEADER_CRC_LEN (32U) /* 覆盖前 32B（magic..entry_offset） */

typedef struct {
    uint32_t signature;      /**< BOOT_IMAGE_SIGNATURE */
    uint32_t magic;          /**< BOOT_IMAGE_MAGIC */
    uint16_t header_version; /**< BOOT_IMAGE_HEADER_VERSION */
    uint16_t slot_id;        /**< 0=A, 1=B */
    uint32_t image_version;  /**< 应用版本号（展示/防回退参考） */
    uint32_t image_size;     /**< payload 字节数（不含头） */
    uint32_t image_crc32;    /**< 覆盖 [槽基址+0x40, +image_size) */
    uint32_t load_addr;      /**< 槽基址 */
    uint32_t entry_offset;   /**< BOOT_IMAGE_ENTRY_OFFSET */
    uint32_t header_crc32;   /**< 覆盖前 32B */
    uint32_t reserved[7];    /**< 0 */
} boot_image_header_t;

_Static_assert(sizeof(boot_image_header_t) == BOOT_IMAGE_HEADER_SIZE,
               "boot_image_header_t must be 64 bytes");

/* ============================================================================
 * 镜像日志（每槽一本，32B 记录，追加写）
 * ============================================================================ */
#define BOOT_IMAGE_REC_MAGIC      (0x494D4554UL) /* "IMET" */
#define BOOT_IMAGE_STATE_FLASHING (0x464C5348UL) /* "FLSH" */
#define BOOT_IMAGE_STATE_READY    (0x52454144UL) /* "READ" */

typedef struct {
    uint32_t magic;      /**< BOOT_IMAGE_REC_MAGIC */
    uint32_t state;      /**< FLASHING / READY */
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t image_version;
    uint32_t reserved[2];
    uint32_t record_crc32; /**< 覆盖前 28B */
} boot_image_record_t;

_Static_assert(sizeof(boot_image_record_t) == 32U, "boot_image_record_t must be 32 bytes");

/* ============================================================================
 * 启动控制日志（16B 记录，追加写）
 * ============================================================================ */
#define BOOT_CONTROL_MAGIC (0x424D4554UL) /* "BMET" */
#define BOOT_SLOT_NONE     (0xFFU)

typedef struct {
    uint32_t magic;    /**< BOOT_CONTROL_MAGIC */
    uint16_t seq;      /**< 单调递增；取最大有效（16 位回绕比较） */
    uint8_t active;    /**< 0=A, 1=B */
    uint8_t pending;   /**< 0=A, 1=B, 0xFF=无 */
    uint8_t attempts;  /**< pending 已尝试启动次数 */
    uint8_t confirmed; /**< 当前 active 是否被应用确认过 */
    uint16_t reserved;
    uint32_t crc32;    /**< 覆盖前 12B */
} boot_control_record_t;

_Static_assert(sizeof(boot_control_record_t) == 16U, "boot_control_record_t must be 16 bytes");

/* ============================================================================
 * 启动邮箱（SYSCTL CPU0 GPR，软复位保持）
 * ============================================================================ */
#define BOOT_MAILBOX_MAGIC       (0x47363631UL) /* "G661" */
#define BOOT_MAILBOX_REQUEST_DFU (0x44465530UL) /* "DFU0" */
#define BOOT_MAILBOX_GPR_INDEX   (12U)

/* ============================================================================
 * Flash 访问后端（bootloader 用 ROM XPI，应用用 intf_flash）
 * ============================================================================ */
typedef struct {
    int (*read)(uint32_t address, void *buf, size_t len);
    int (*erase_sector)(uint32_t address);
    int (*program)(uint32_t address, const void *buf, size_t len);
} boot_flash_ops_t;

/* ============================================================================
 * 接口
 * ============================================================================ */

/** @brief IEEE CRC32（poly 0x04C11DB7，init/xorout 0xFFFFFFFF，反射） */
uint32_t boot_crc32(const void *data, size_t len);

/** @brief 槽基址 / 镜像日志扇区 由槽号派生（slot 必须为 0/1） */
uint32_t boot_slot_address(uint8_t slot_id);
uint32_t boot_slot_meta_address(uint8_t slot_id);

/**
 * @brief 校验镜像头与槽是否匹配（不校验 payload CRC）
 * @param hdr 镜像头（槽基址）
 * @param slot_id 目标槽 0/1
 * @return true = 头合法且属于目标槽
 */
bool boot_image_header_valid(const boot_image_header_t *hdr, uint8_t slot_id);

/**
 * @brief 读取某槽最新一条记录
 * @return 0 = 找到有效记录；-1 = 无记录或最后一条损坏
 */
int boot_image_record_load(const boot_flash_ops_t *ops, uint32_t sector_addr,
                           boot_image_record_t *out);

/** @brief 追加一条 FLASHING 记录（开始写入镜像） */
int boot_image_record_begin(const boot_flash_ops_t *ops, uint32_t sector_addr,
                            uint32_t image_size, uint32_t image_version);

/** @brief 追加一条 READY 记录（镜像写入并校验完成） */
int boot_image_record_ready(const boot_flash_ops_t *ops, uint32_t sector_addr,
                            uint32_t image_size, uint32_t image_crc32, uint32_t image_version);

/** @brief 读取最新一条启动控制记录；0 = 找到，-1 = 无有效记录 */
int boot_control_load(const boot_flash_ops_t *ops, boot_control_record_t *out);

/** @brief 追加一条启动控制记录（seq 自动 +1） */
int boot_control_save(const boot_flash_ops_t *ops, const boot_control_record_t *rec);

/** @brief 无控制记录时的默认值（active=A, pending=NONE） */
void boot_control_defaults(boot_control_record_t *rec);

/**
 * @brief 应用启动确认：pending == slot_id 时写 active=slot_id/pending=NONE
 * @return 0 = 已确认或无需确认；-1 = flash 写失败
 *
 * 幂等：控制记录已是目标状态时不再追加（避免每次 boot 写 flash）。
 */
int boot_confirm_running_slot(const boot_flash_ops_t *ops, uint8_t slot_id);

/** @brief 写启动邮箱请求 DFU（不复位；复位由调用方执行） */
void boot_mailbox_request_dfu(void);

/** @brief 消费 DFU 请求（读取后清零；true = 本次复位来自 DFU 请求） */
bool boot_mailbox_consume_dfu(void);

/** @brief 软件复位（PPOR；不返回） */
void boot_system_reset(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* INTF_BOOT_H */
