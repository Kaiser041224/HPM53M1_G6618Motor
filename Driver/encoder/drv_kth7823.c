/**
 * @file    drv_kth7823.c
 * @brief   KTH7823 磁编码器驱动（SPI 协议，每实例设备对象）
 * @author  Kaiser
 *
 * 协议要点（依据 KTH7823 数据手册）：
 *   - SPI mode3（CPOL=1/CPHA=1），16bit 帧，MSB first，SCK ≤10MHz（TSCK≥100ns）
 *   - 重叠结构：每帧一个 CS 周期，第 N 帧的响应随第 N+1 帧返回
 *   - 读角度：opcode 000（MOSI 保持低）→ 帧1 发命令，帧2 取 16bit 响应
 *   - 读寄存器：帧1 = 01 + ADR[5:0] + 8×0；帧2 响应高 8 位为寄存器值
 *   - 写寄存器：帧1 = 10 + ADR[5:0] + WRD[7:0]；帧间等待 ≥20ms（MTP 烧写）；
 *     帧2 为确认帧（高 8 位 = 新写入值）
 *   - 两帧间隔（Tpause>150ns）由 SPI 硬件 csht 保证（见 drv_spi）
 *   - 本驱动不含任何 hpm_* 头文件，只依赖 intf_spi / intf_encoder / intf_clock
 *
 * 注意：寄存器写操作烧写 MTP（寿命 1000 次），只能用于一次性配置；
 *       零点（Z，0x00/0x01）与方向（RD，0x09 bit7）配置各消耗 1~2 次 MTP 写。
 *
 * 实例：双编码器设计（0 = 转子 / 1 = 出轴，由平台层映射总线）
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "intf_encoder.h"
#include "intf_spi.h"
#include "intf_clock.h"

#define KTH7823_INSTANCE_COUNT (2U)        /* 双编码器设计 */
#define KTH7823_SCLK_MAX_HZ    (10000000U) /* TSCK ≥ 100ns */
#define KTH7823_SPI_TIMEOUT_MS (1U)        /* 单帧内部超时（标称 ~5µs） */
#define KTH7823_MTP_DELAY_MS   (20U)       /* 写寄存器帧间最小等待 */

/* 帧编码 */
#define KTH7823_CMD_READ_ANGLE (0x0000U) /* 000 + 任意（MOSI 保持低） */
#define KTH7823_CMD_READ_REG   (0x4000U) /* 01 + ADR[5:0] + 8×0 */
#define KTH7823_CMD_WRITE_REG  (0x8000U) /* 10 + ADR[5:0] + WRD[7:0] */

/* 寄存器地址 */
#define KTH7823_REG_Z_LOW    (0x00U) /* Z(7:0)  */
#define KTH7823_REG_Z_HIGH   (0x01U) /* Z(15:8) */
#define KTH7823_REG_RD       (0x09U) /* 旋转方向：RD 位于 bit7，出厂默认 1（0x80） */
#define KTH7823_REG_RD_BIT   (0x80U)
#define KTH7823_REG_ADDR_MAX (0x3FU) /* 6bit 地址 */

/**
 * @brief KTH7823 实例上下文
 */
typedef struct {
    const intf_spi_t *spi;      /**< 总线设备对象（init 时解析并缓存） */
    uint32_t error_count;       /**< 通信错误累计 */
    bool initialized;           /**< 是否已初始化 */
} kth7823_ctx_t;

static kth7823_ctx_t s_kth7823_ctx[KTH7823_INSTANCE_COUNT];

/* ============================================================================
 * 帧级收发
 * ============================================================================ */

/**
 * @brief 发送一帧并接收一帧（一个 CS 周期）；失败累计错误计数
 * @param ctx 实例上下文
 * @param tx_word 发送字
 * @param rx_word 接收字输出
 * @return 0 = 成功；-1 = SPI 传输失败
 */
static int kth7823_frame(kth7823_ctx_t *ctx, uint16_t tx_word, uint16_t *rx_word)
{
    uint16_t received_word;

    if (ctx->spi->transfer(&tx_word, &received_word, 1U, KTH7823_SPI_TIMEOUT_MS) != 0) {
        ctx->error_count++;
        return -1;
    }
    *rx_word = received_word;

    return 0;
}

/* ============================================================================
 * 实现（按实例）
 * ============================================================================ */

/**
 * @brief 初始化指定编码器实例
 * @param id 实例号
 * @param cfg 编码器配置
 * @return 0 = 成功；-1 = 参数非法或总线未注册
 */
static int kth7823_init_impl(uint8_t id, const intf_encoder_cfg_t *cfg)
{
    intf_spi_cfg_t spi_cfg;
    kth7823_ctx_t *ctx;

    if ((id >= KTH7823_INSTANCE_COUNT) || (cfg == NULL)) {
        return -1;
    }
    if ((cfg->sclk_hz == 0U) || (cfg->sclk_hz > KTH7823_SCLK_MAX_HZ)) {
        return -1; /* 超出器件上限 */
    }

    ctx = &s_kth7823_ctx[id];
    ctx->spi = intf_spi_get(cfg->bus);
    if ((ctx->spi == NULL) || (ctx->spi->init == NULL) || (ctx->spi->transfer == NULL)) {
        return -1; /* 总线未注册 */
    }

    spi_cfg.sclk_hz = cfg->sclk_hz;
    spi_cfg.cpol = 1U;      /* mode3 */
    spi_cfg.cpha = 1U;
    spi_cfg.data_bits = 16U;
    spi_cfg.cs_index = 0U;  /* CS0 */

    if (ctx->spi->init(&spi_cfg) != 0) {
        return -1;
    }

    ctx->error_count = 0U;
    ctx->initialized = true;

    return 0;
}

/**
 * @brief 反初始化指定编码器实例
 * @param id 实例号
 */
static void kth7823_deinit_impl(uint8_t id)
{
    if (id >= KTH7823_INSTANCE_COUNT) {
        return;
    }
    s_kth7823_ctx[id].initialized = false;
}

/**
 * @brief 读取原始角度（两帧重叠协议）
 * @param id 实例号
 * @param raw 原始角度输出
 * @return 0 = 成功；-1 = 参数非法或未初始化
 */
static int kth7823_read_raw_impl(uint8_t id, uint16_t *raw)
{
    kth7823_ctx_t *ctx;
    uint16_t response;

    if ((id >= KTH7823_INSTANCE_COUNT) || (raw == NULL)) {
        return -1;
    }

    ctx = &s_kth7823_ctx[id];
    if (!ctx->initialized) {
        return -1;
    }

    /* 帧1：读角度命令（响应随帧2 返回，帧1 响应为上一条命令的残留，丢弃） */
    if (kth7823_frame(ctx, KTH7823_CMD_READ_ANGLE, &response) != 0) {
        return -1;
    }
    /* 帧2：取本次角度响应 */
    if (kth7823_frame(ctx, KTH7823_CMD_READ_ANGLE, raw) != 0) {
        return -1;
    }

    return 0;
}

/**
 * @brief 读取寄存器
 * @param id 实例号
 * @param addr 寄存器地址
 * @param val 寄存器值输出
 * @return 0 = 成功；-1 = 参数非法或未初始化
 */
static int kth7823_read_reg_impl(uint8_t id, uint8_t addr, uint8_t *val)
{
    kth7823_ctx_t *ctx;
    uint16_t response;

    if ((id >= KTH7823_INSTANCE_COUNT) || (val == NULL) || (addr > KTH7823_REG_ADDR_MAX)) {
        return -1;
    }

    ctx = &s_kth7823_ctx[id];
    if (!ctx->initialized) {
        return -1;
    }

    if (kth7823_frame(ctx, (uint16_t)(KTH7823_CMD_READ_REG | ((uint16_t) addr << 8)),
                      &response) != 0) {
        return -1;
    }
    if (kth7823_frame(ctx, KTH7823_CMD_READ_ANGLE, &response) != 0) {
        return -1;
    }

    *val = (uint8_t)(response >> 8);
    return 0;
}

/**
 * @brief 写寄存器（烧写 MTP，帧间等待 ≥20ms）
 * @param id 实例号
 * @param addr 寄存器地址
 * @param val 写入值
 * @return 0 = 成功；-1 = 参数非法、未初始化或确认帧不匹配
 */
static int kth7823_write_reg_impl(uint8_t id, uint8_t addr, uint8_t val)
{
    kth7823_ctx_t *ctx;
    uint16_t response;

    if ((id >= KTH7823_INSTANCE_COUNT) || (addr > KTH7823_REG_ADDR_MAX)) {
        return -1;
    }

    ctx = &s_kth7823_ctx[id];
    if (!ctx->initialized) {
        return -1;
    }

    /* 帧1：写请求（10 + ADR + WRD） */
    if (kth7823_frame(ctx, (uint16_t)(KTH7823_CMD_WRITE_REG | ((uint16_t) addr << 8) | val),
                      &response) != 0) {
        return -1;
    }

    /* MTP 烧写窗口：帧1 与帧2 之间必须 ≥20ms */
    intf_clock_delay_ms(KTH7823_MTP_DELAY_MS);

    /* 帧2：确认帧（高 8 位 = 新写入值） */
    if (kth7823_frame(ctx, KTH7823_CMD_READ_ANGLE, &response) != 0) {
        return -1;
    }

    return ((uint8_t)(response >> 8) == val) ? 0 : -1;
}

/**
 * @brief 设置零点（Z 跨两个寄存器，各消耗一次 MTP 写）
 * @param id 实例号
 * @param zero 零点原始值
 * @return 0 = 成功；-1 = 写入失败
 */
static int kth7823_set_zero_impl(uint8_t id, uint16_t zero)
{
    if (kth7823_write_reg_impl(id, KTH7823_REG_Z_LOW, (uint8_t)(zero & 0xFFU)) != 0) {
        return -1;
    }
    return kth7823_write_reg_impl(id, KTH7823_REG_Z_HIGH, (uint8_t)(zero >> 8));
}

/**
 * @brief 设置旋转方向
 * @param id 实例号
 * @param cw_increasing true = 顺时针角度增加
 * @return 0 = 成功；-1 = 写入失败
 */
static int kth7823_set_direction_impl(uint8_t id, bool cw_increasing)
{
    /* RD 位于寄存器 0x09 的 bit7：1 = 俯视顺时针（CW）角度增加（出厂默认 0x80） */
    return kth7823_write_reg_impl(id, KTH7823_REG_RD,
                                  cw_increasing ? KTH7823_REG_RD_BIT : 0x00U);
}

/**
 * @brief 获取编码器信息（分辨率、寄存器支持）
 * @param id 实例号
 * @param info 信息输出
 * @return 0 = 成功；-1 = 参数非法
 */
static int kth7823_get_info_impl(uint8_t id, intf_encoder_info_t *info)
{
    if ((id >= KTH7823_INSTANCE_COUNT) || (info == NULL)) {
        return -1;
    }

    info->resolution_bits = 16U;
    info->has_registers = true;
    return 0;
}

/**
 * @brief 获取通信错误累计计数
 * @param id 实例号
 * @return 错误计数；实例越界返回 0
 */
static uint32_t kth7823_get_error_count_impl(uint8_t id)
{
    if (id >= KTH7823_INSTANCE_COUNT) {
        return 0U;
    }
    return s_kth7823_ctx[id].error_count;
}

/* ============================================================================
 * 每实例设备对象（风格 A）
 * ============================================================================ */

/**
 * @brief 编码器实例 0 初始化包装
 * @param cfg 编码器配置
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_init(const intf_encoder_cfg_t *cfg) { return kth7823_init_impl(0U, cfg); }

/**
 * @brief 编码器实例 0 反初始化包装
 */
static void enc0_deinit(void) { kth7823_deinit_impl(0U); }

/**
 * @brief 编码器实例 0 读取原始角度
 * @param raw 原始角度输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_read_raw(uint16_t *raw) { return kth7823_read_raw_impl(0U, raw); }

/**
 * @brief 编码器实例 0 读取寄存器
 * @param addr 寄存器地址
 * @param val 寄存器值输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_read_reg(uint8_t addr, uint8_t *val) { return kth7823_read_reg_impl(0U, addr, val); }

/**
 * @brief 编码器实例 0 写寄存器
 * @param addr 寄存器地址
 * @param val 写入值
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_write_reg(uint8_t addr, uint8_t val) { return kth7823_write_reg_impl(0U, addr, val); }

/**
 * @brief 编码器实例 0 设置零点
 * @param zero 零点原始值
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_set_zero(uint16_t zero) { return kth7823_set_zero_impl(0U, zero); }

/**
 * @brief 编码器实例 0 设置方向
 * @param cw 顺时针角度增加
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_set_direction(bool cw) { return kth7823_set_direction_impl(0U, cw); }

/**
 * @brief 编码器实例 0 获取信息
 * @param info 信息输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc0_get_info(intf_encoder_info_t *info) { return kth7823_get_info_impl(0U, info); }

/**
 * @brief 编码器实例 0 获取错误计数
 * @return 错误计数
 */
static uint32_t enc0_get_error_count(void) { return kth7823_get_error_count_impl(0U); }

/**
 * @brief 编码器实例 1 初始化包装
 * @param cfg 编码器配置
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_init(const intf_encoder_cfg_t *cfg) { return kth7823_init_impl(1U, cfg); }

/**
 * @brief 编码器实例 1 反初始化包装
 */
static void enc1_deinit(void) { kth7823_deinit_impl(1U); }

/**
 * @brief 编码器实例 1 读取原始角度
 * @param raw 原始角度输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_read_raw(uint16_t *raw) { return kth7823_read_raw_impl(1U, raw); }

/**
 * @brief 编码器实例 1 读取寄存器
 * @param addr 寄存器地址
 * @param val 寄存器值输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_read_reg(uint8_t addr, uint8_t *val) { return kth7823_read_reg_impl(1U, addr, val); }

/**
 * @brief 编码器实例 1 写寄存器
 * @param addr 寄存器地址
 * @param val 写入值
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_write_reg(uint8_t addr, uint8_t val) { return kth7823_write_reg_impl(1U, addr, val); }

/**
 * @brief 编码器实例 1 设置零点
 * @param zero 零点原始值
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_set_zero(uint16_t zero) { return kth7823_set_zero_impl(1U, zero); }

/**
 * @brief 编码器实例 1 设置方向
 * @param cw 顺时针角度增加
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_set_direction(bool cw) { return kth7823_set_direction_impl(1U, cw); }

/**
 * @brief 编码器实例 1 获取信息
 * @param info 信息输出
 * @return 0 = 成功；-1 = 失败
 */
static int enc1_get_info(intf_encoder_info_t *info) { return kth7823_get_info_impl(1U, info); }

/**
 * @brief 编码器实例 1 获取错误计数
 * @return 错误计数
 */
static uint32_t enc1_get_error_count(void) { return kth7823_get_error_count_impl(1U); }

static const intf_encoder_t s_enc0_dev = {
    .instance_id = 0U,
    .init = enc0_init,
    .deinit = enc0_deinit,
    .read_raw = enc0_read_raw,
    .read_reg = enc0_read_reg,
    .write_reg = enc0_write_reg,
    .set_zero = enc0_set_zero,
    .set_direction = enc0_set_direction,
    .get_info = enc0_get_info,
    .get_error_count = enc0_get_error_count,
};

static const intf_encoder_t s_enc1_dev = {
    .instance_id = 1U,
    .init = enc1_init,
    .deinit = enc1_deinit,
    .read_raw = enc1_read_raw,
    .read_reg = enc1_read_reg,
    .write_reg = enc1_write_reg,
    .set_zero = enc1_set_zero,
    .set_direction = enc1_set_direction,
    .get_info = enc1_get_info,
    .get_error_count = enc1_get_error_count,
};

/* ============================================================================
 * 注册
 * ============================================================================ */

void hpm_kth7823_driver_register(void)
{
    intf_encoder_register(&s_enc0_dev);
    intf_encoder_register(&s_enc1_dev);
}
