# SPI 总线 + 编码器驱动设计（intf_spi / intf_encoder / KTH7823）

- 日期：2026-09-18
- 状态：**已批准**（Kaiser 2026-09-18，方案 A：总线 intf/drv + 设备 intf/drv）
- 关联：`2026-09-18-encoder-sensing-design.md`（机械/游标）、`2026-09-18-m1-board-bringup-design.md`（硬件事实）
- 范围：SPI 总线驱动、KTH7823 编码器驱动、平台封装、自检；**不含**游标解算与 FOC 集成

## 1. 分层与文件

```
Interface/intf_spi.h            SPI 主机契约（总线层）
Driver/hpm_impl/drv_spi.c       HPM SPI1/SPI3 适配（MCU 外设驱动）
Interface/intf_encoder.h        编码器契约（设备层）
Driver/encoder/drv_kth7823.c    KTH7823 协议实现（器件驱动，只依赖 intf_spi / intf_clock）
App/Platform/app_encoder.*      实例管理、单位换算、零点、寄存器/方向门面
App/Platform/app_param.*        flash 键值参数存储（通用；零点等参数落盘）
App/Debug/app_debug_encoder.*   自检
```

**目录约定**：`Driver/hpm_impl/` 放 HPM MCU 外设驱动；`Driver/encoder/` 放编码器器件驱动
（不同编码器并列新增，互不影响）；器件驱动只依赖 `Interface/` 契约。

换编码器 = 新增 `drv_xxx.c` 实现 `intf_encoder` + 注册；换 SPI 控制器 = 换 `drv_spi.c`。
编码器驱动不含任何 `hpm_*` 头文件。

## 2. 实例与总线

| 实例 | 总线 | 引脚 | 机械关系 |
| :--- | :--- | :--- | :--- |
| `APP_ENCODER_ROTOR = 0` | SPI3 | PA10-13（CS0/SCLK/MISO/MOSI） | 49:49，1:1 |
| `APP_ENCODER_OUTPUT = 1` | SPI1 | PA26-29（CS0/SCLK/MISO/MOSI） | 49:50，游标 |

每路独占总线，均使用 CS0；SCLK 10MHz（器件上限，TSCK≥100ns）。

## 3. 契约

### 3.1 `intf_spi`（设备对象，风格 A）

```c
typedef struct {
    uint32_t sclk_hz;   /* 期望 SCLK（drv 取整数分频，失败返回 -1） */
    uint8_t  cpol;      /* 0=空闲低 / 1=空闲高 */
    uint8_t  cpha;      /* 0=首边沿采样 / 1=次边沿采样（mode3 = 1/1） */
    uint8_t  data_bits; /* 每帧位数 1..32 */
    uint8_t  cs_index;  /* 0..3 -> CS0..CS3 */
} intf_spi_cfg_t;

typedef struct {
    uint8_t instance_id;               /* 总线实例：0..3 -> SPI0..SPI3 */
    struct {
        int      (*init)(const intf_spi_cfg_t *cfg);
        int      (*transfer)(const void *tx, void *rx, size_t frames, uint32_t timeout_ms);
        void     (*deinit)(void);
        uint32_t (*get_sclk_hz)(void);
    };
} intf_spi_t;

int intf_spi_register(const intf_spi_t *dev);
const intf_spi_t *intf_spi_get(intf_spi_bus_t bus);
```

- **一次 transfer = 一个 CS 周期**；CS 由控制器硬件自动控制。
- 全双工，`tx`/`rx` 元素宽度 = `data_bits/8` 字节（16bit → uint16_t）。
- `timeout_ms` 语义与其他驱动一致：0=不等待 / `UINT32_MAX`=无限 / 毫秒。
- 忙（`status_spi_master_busy`）按超时重试；其他错误立即失败。
- 驱动为每实例生成薄包装 + const 设备对象（照 `drv_mcan` 模式）；
  消费者 `intf_spi_get(bus)->transfer(...)`，`drv_kth7823` 在 init 时解析并缓存对象指针。

### 3.2 `intf_encoder`（设备对象，风格 A）

```c
typedef struct { uint8_t bus; uint32_t sclk_hz; } intf_encoder_cfg_t;
typedef struct { uint8_t resolution_bits; bool has_registers; } intf_encoder_info_t;

typedef struct {
    uint8_t instance_id;               /* 编码器实例（平台层映射转子/出轴） */
    struct {
        int      (*init)(const intf_encoder_cfg_t *cfg);
        void     (*deinit)(void);
        int      (*read_raw)(uint16_t *raw);
        int      (*read_reg)(uint8_t addr, uint8_t *val);
        int      (*write_reg)(uint8_t addr, uint8_t val);
        int      (*set_zero)(uint16_t zero);
        int      (*set_direction)(bool cw_increasing);
        int      (*get_info)(intf_encoder_info_t *info);
        uint32_t (*get_error_count)(void);
    };
} intf_encoder_t;

int intf_encoder_register(const intf_encoder_t *dev);
const intf_encoder_t *intf_encoder_get(intf_encoder_id_t id);
```

- 契约只含语义操作，不含 KTH7823 寄存器地址；`resolution_bits` 支持换不同分辨率器件。
- **每实例单所有者**：不可在多上下文并发调用。
- `read_raw`：阻塞、无打印、无动态分配、固定内部超时（1ms）+ 错误计数。

> 风格说明（2026-09-18）：本模块按 `AGENTS.md §3.1` 采用"设备对象 + 匿名结构体"
> （与 `intf_can` / `intf_hrpwm` 一致）；`uart` / `usb` 已完成同风格对齐（2026-09-19），
> 其余设备契约均已为对象风格。

## 4. KTH7823 协议实现（依据数据手册，图 9 已核实）

- mode3（CPOL=1/CPHA=1），16bit 帧，MSB first。
- **重叠结构**：每帧一个 CS 周期，第 N 帧响应随第 N+1 帧返回 → 每次读 = 2 次 transfer。
- 读角度：帧1 `000`+任意（MOSI 保持低）→ 帧2 取 16bit 响应。
- 读寄存器：帧1 `01|ADR[5:0]|8×0`（0x4000|addr<<8）→ 帧2 响应 [15:8] 为值。
- 写寄存器：帧1 `10|ADR|WRD`（0x8000|addr<<8|val）→ **≥20ms（MTP 烧写）** → 帧2 确认（校验 [15:8]）。
- Tpause>150ns 由 SPI 硬件 `csht`（默认 12 半 SCLK，@10MHz=600ns）保证。
- 寄存器：0x00/0x01 = Z(7:0)/Z(15:8)、0x04/0x05 = PPT/ZL/ZD、0x06 = mgh/mgl、
  0x08 = ABZLIMIT、0x09 = RD（**位于 bit7**，出厂默认 1 → 读回 `0x80`）。
  （RD 位定义由数据手册 p.22 位图核实；`set_direction` 写 0x80/0x00。）
- **MTP 寿命 1000 次写**：`set_zero`（两次写）/`set_direction`（一次写）仅一次性配置；
  自检与周期路径只读。

## 5. 平台层与实时性

- `app_encoder_read_raw/rad/deg`、`read_reg/write_reg/set_zero/set_direction`、
  `get_error_count`、`get_sclk_hz`。
- 单位换算按实例分辨率在 init 时预计算（无运行期除法）。
- 单次读标称 ~5µs（2×16bit @10MHz + CS 间隙 + SDK 开销），双路 ~10µs；
  20kHz 环路占比可接受；未来如需异步可在 `intf_encoder` 增加 `start/result` 操作（本阶段不做）。
- 双路顺序采样 ~10µs 间隔，对游标差值动态影响可忽略（差值变化率 = 转速/50）。
- **25kHz 控制环仿真（M1 bring-up）**：主循环按 40µs 节拍运行
  （主循环节拍 = inverter.pwm_freq_hz，config/hardware.yaml），每周期采样转子；
  1Hz 汇总打印实际速率 / 迟到计数与最大迟到 / 单次读耗时 avg·max / 错误计数。
  UART/CAN/USB 调试任务降为 1ms 分频，心跳轮次不计入迟到统计。
- **读路径优化（实测驱动）**：SDK `spi_transfer` 每次调用做 FIFO/控制器复位，
  实测单路读 14µs（双路 28µs = 70% @25kHz）→ `drv_spi` 增加**快速单帧路径**：
  init 预置 TRANSCTRL/CS_EN，每帧"等空闲 → **写 CMD（触发，必须）** → 写 DATA
  → 等 RX → 读 DATA → 等 CS 释放"；frames > 1 或快速路径失败时回退 SDK 路径。
  **IP 要点**：HPM SPI 主机传输由 **CMD 寄存器写入触发**（SDK 文档："the command value
  must be set before transmission"；IP 文档："SPIActive becomes 1 after the SPI command
  register is written"）。漏写 CMD 会导致传输不启动、RX 等待超时（实测 1.5ms/帧）。
  **实测结果（2026-09-18）**：单路读 14µs（SDK）→ **7µs**（快速路径，3343 cycles @480MHz，
  含 2 帧）；转子 25kHz 采样占用 ~17.5% CPU；双路 25kHz 压力模式 ~35%。
- **出轴降采样**：`ENC_OUTPUT_SAMPLE_DIV = 25`（转子 25kHz / 出轴 1kHz）；
  游标差值变化率仅为转子 1/50，1kHz 足够；设 1 可回到双路 25kHz 压力模式。

## 6. 自检与验证

1. 初始化双路 → 读 RD（0x09）期望 `0x01`（寄存器读通路 + 器件在线）。
2. mcycle 实测单次 `read_raw` 耗时并打印。
3. 1Hz 打印双路 `raw + deg + err`（整数 mdeg 打印，避免 newlib %f 依赖）。
4. 手动转动转子：转子编码器 1:1、出轴编码器 49:50 变化。
5. 全程不写寄存器。
6. Ozone 观测：每控制周期更新 `.noncacheable.bss` 全局变量（定义于 `app_debug_encoder.c`）：
   `g_enc_rotor_raw/deg`、`g_enc_output_raw/deg`（角度）、
   `g_enc_rotor_read_us`、`g_enc_output_read_us`（单次读耗时）、
   `g_enc_loop_late_us`（节拍迟到）；25kHz 仿真下刷新率 = 25kHz。
7. 25kHz 仿真判据（实测 2026-09-18 通过）：`rate` ≈ 24940 Hz、`late` ≈ 2-3/s
   （UART 1Hz 状态行阻塞，属 bring-up 固件特有）、单路读 avg 7µs / max 23-32µs
   （ISR 抢占尖峰，< 40µs 预算）、`err` = 0/0、RD = 1（bit7）。

## 7. 后续（不在本阶段）

- 游标多圈解算（见 sensing 设计文档）。
- FOC 集成：角度读取进控制环、异步/DMA 读优化（如需）。
- 零点/方向标定（一次性 MTP 写，标定流程单独设计）。
