# M1 设计：板级硬件描述 + 底层驱动 + 参数管线

- 日期：2026-09-18
- 项目：HPM53M1_G6618Motor
- 状态：待 Kaiser 审阅
- 依据资料：
  - `Doc/datasheet/HPM53M1DSV0.31.pdf`（HPM53M1 Rev0.3）
  - `Doc/datasheet/HPM-FOC_G6618.pdf`（原理图 HPM5361_FOC Rev0.1，KiCad）
  - `Doc/datasheet/G6618 motor.pdf`（舞肌 G66-18）
  - `Doc/datasheet/encoder_kth7823.pdf`（KTH7823）
  - `Doc/datasheet/pinmux.c`（HPM Pinmux Tool 输出）

## 1. 目标与范围

本阶段只做三件事，不做 FOC 控制算法：

1. **板级硬件描述**：把原理图事实固化到 Board 层与平台层（pinmux 已完成，本阶段补常量与时钟）。
2. **底层驱动实现**：UART、SPI、ADC 采样链、PWM1 三相、CAN、编码器。
3. **YAML 参数管线**：编译前由 YAML 提供电机默认参数（方案 B），为后续在线辨识/Flash 覆盖预留结构。

成功标准：可编译、可烧录；UART 有输出；时钟 480MHz 生效；各驱动有可执行的最小自检（详见 §8）。

## 2. 硬件事实

### 2.1 芯片与功率级
- MCU：**HPM53M1IN91**（QFN80 9×9，1MB Flash，24MHz 晶振，RV32-IMAFDCBP @480MHz）。
- 三相预驱为**芯片内部**（datasheet §1.3.7："PWM1 定时器已内部连接到合封的三相半桥驱动"），**无外部栅极驱动器**（首页 BOM 中 FD6288 为 REV0.1 旧文本，原理图实际走内部预驱）。
- 功率 MOSFET：12× CRSM038N10N4（100V/100A/3mΩ），栅极 5.1Ω + 10kΩ 下拉。
- 预驱供电：U6 SGM6602-12（+5V→+12V），EN = `DRV_+12V_EN`（PA09）。**硬件现状**：原理图为 15K 下拉（默认关闭）；当前测试板将 15K 焊至 5V 作为上拉（默认使能），定稿改回下拉。固件当前按上拉状态配置为开漏；定稿改下拉后需切换为推挽输出。
- 自举：VB1/2/3 + 1µF；MD12V_VDD(59)/MD12V_GND(58)。

### 2.2 采样链
- 相电流：**三电阻低侧采样**，Rshunt 2mΩ + **TPA6584Q 四运放**，增益 7.5，偏置 1.65V，量程约 ±100A。
- 母线电压：电阻分压 → **芯片内部独立运放 B 缓冲** → V_VBUS（内部运放 A 未用；MCU 可编程 OPAMP 未用，无需软件配置）。
- 温度：两路 NTC 上拉分压（10K 1% 上拉 + 1µF）。
- CAN ID：SW2 四路 DIP + 电阻网络 → 模拟电压 `ADC_V_CANID`（PB00），固件通过 ADC 读取电压判断开关状态（实现暂缓）。
- 模拟基准：U7 TPR3533 独立 3.3V LDO 产生 `VREF_3.3V`。

### 2.3 通信与外设
- CAN：MCAN3 → TPT1044VQ（+5V 供电），120Ω 端接由 DIP 开关 `RES_CTL` 控制（无 MCU 引脚）。
- 编码器：**双 KTH7823**（16bit 绝对角度，SPI mode 3，SCK ≤10MHz，16 位帧；数据格式 16bit 原码、无 CRC——已确认）：
  - 转子端 → **SPI3**（PA10–PA13）
  - 输出端 → **SPI1**（PA26–PA29）
  - A/B/Z 与 PWM 输出未接（纯 SPI）。
- UART0：上位机调参 + ISP（PA00/PA01，115200）。
- 调试：JTAG（PA04–PA08）；USB 仅做了 ESD/共模（不用于业务）。

## 3. 引脚映射总表（= Board pinmux 现状）

| 功能 | Pad | 配置 | 备注 |
|---|---|---|---|
| UART0_TX / RX | PA00 / PA01 | UART0 | J10，33R/12R 串阻 |
| BOOT0 / BOOT1 | PA02 / PA03 | 输入（strap） | R5/R6 10K |
| JTAG TDO/TDI/TCK/TMS/TRST | PA04–PA08 | JTAG | J8 |
| DRV_+12V_EN | PA09 | GPIO 输出，**开漏**（当前） | 测试板 15K 上拉：0=关，1=释放；定稿 15K 下拉后改推挽 |
| SPI3（Rotor 编码器） | PA10/11/12/13 = CS0/SCLK/MISO/MOSI | SPI3 | KTH7823 U13 |
| MCAN3 RX/TX | PA14 / PA15 | MCAN3 | TPT1044VQ |
| 内部预驱 HIN1/LIN1 | PA20 / PA21 | PWM1_P_4 / P_5 | pad 不引出，仅 IOC |
| 内部预驱 HIN2/LIN2 | PA22 / PA23 | PWM1_P_6 / P_7 | 同上 |
| 内部预驱 HIN3/LIN3 | PA24 / PA25 | PWM1_P_0 / P_1 | 同上 |
| SPI1（Output 编码器） | PA26/27/28/29 = CS0/SCLK/MISO/MOSI | SPI1 | KTH7823 U14 |
| V_CANID | PB00 | 模拟 | ADC_IN15，DIP 编码 |
| LED | PB01 | GPIO 输出 | 低有效（D9+R13 10K） |
| NTC1 | PB08 | 模拟 | ADC_IN11；原理图网络名 V_NTC0 与连接器交叉，以连接器为准 |
| NTC0 | PB09 | 模拟 | ADC_IN1；原理图网络名 V_NTC1 与连接器交叉，以连接器为准 |
| I_W | PB10 | 模拟 | ADC0_IN2 |
| I_U | PB11 | 模拟 | ADC0_IN3 |
| I_V | PB12 | 模拟 | ADC0_IN4 |
| V_VBUS | PB14 | 模拟 | ADC1_IN6（内部运放 B 输出） |
| — | PB13 | — | 未连接 |

> 注：PB00/PB01 为 Mephisto 对照原理图后**补充**到 pinmux 的（工具输出缺失）。

## 4. 软件落点

- `Board/HPM53M1_G6618Motor_board/`：pinmux（已完成）、board.h 板级常量、board.c flash 选项。
- `App/Platform/`：各模块引脚宏与能力封装（app_gpio 已更新，app_adc / app_hrpwm / 新 app_spi 待更新）。
- `Driver/hpm_impl/`：驱动实现。
- `App/Control/`（新建）：`motor_params`（YAML 生成的默认参数）。
- `App/Logic/app_logic.c`：`app_init()` 首行调用 `intf_clock_init()`（顺序 `board_init` → `intf_clock_init`，与 SuperCap 工程一致）。

## 5. 驱动实现清单

| 优先级 | 模块 | 内容 | 接口 |
|---|---|---|---|
| P0 | drv_uart | UART0 115200，轮询 TX + 中断 RX，提供 UART 调试输出（与 RTT 并存） | 现有 `intf_uart` |
| P0 | 时钟接线 | main 调用 `intf_clock_init()`；打印 CPU/AHB 频率自检 | `intf_clock` |
| P1 | drv_spi（新建） | SPI1/SPI3，mode 3，≤10MHz，16 位帧，阻塞收发 + 片选控制 | 新建 `intf_spi` |
| P1 | ADC 链重写 | 7 通道映射（§6.1）；PMT 触发源 = PWM1 CMP → TRGM；DMA 缓存 | 现有 `intf_adc` |
| P1 | PWM1 三相 | 扩展通道表覆盖 PWM1 ch0/1；三相接口；死区策略；输出使能/急停 | `intf_hrpwm` + app_hrpwm |
| P2 | MCAN3 验证 | 1Mbps 收发 + 回环测试（app_can 已切实例） | 现有 `intf_can` |
| P2 | KTH7823 驱动 | 转子/输出双编码器：角度读取（opcode 000）、寄存器读写、零位/方向配置 | 新建 `app_encoder` |
| P2 | CANID 读取（暂缓） | PB00 模拟采样 → 电阻网络解码表 | app_adc |

## 6. 关键设计

### 6.1 ADC 通道与触发
| 逻辑通道 | Pad | ADC 实例/通道 | 物理量 |
|---|---|---|---|
| I_U | PB11 | ADC0_IN3 | 相电流 U |
| I_V | PB12 | ADC0_IN4 | 相电流 V |
| I_W | PB10 | ADC0_IN2 | 相电流 W |
| V_VBUS | PB14 | ADC1_IN6 | 母线电压 |
| NTC1 | PB08 | ADC1_IN11 | 温度通道（连接器 NTC1） |
| NTC0 | PB09 | ADC1_IN1 | 温度通道（连接器 NTC0） |
| V_CANID | PB00 | ADC0/1_IN15（待定实例） | CAN ID 编码 |

- 三路相电流均在 ADC0 → **顺序采样**（同一触发内按 slot 依次转换）；M1 需实测采样间隔是否满足电流环需求。
- 采样时刻：低侧三电阻，应在 PWM 下溢/低侧导通窗口触发（中心对齐）；触发链 `PWM1 CMP → TRGM → ADC PMT`。
- 换算：`I = (Vadc − 1.65V) / 7.5 / 2mΩ`（标定后落 `app_analog_signal`）。

### 6.2 PWM1 三相
- 映射：**U→PWM1_P_4/5（HIN1/LIN1）、V→PWM1_P_6/7（HIN2/LIN2）、W→PWM1_P_0/1（HIN3/LIN3）**。
- 平台缺口：现 `hrpwm` 通道表为 `{0,2,4,6}`（PWM1 侧仅覆盖 ch4–7），需扩展以支持 PWM1 ch0/1。
- 死区：内部预驱自带 50–250ns 死区（datasheet 表 32），HPM 侧建议设小死区（≤100ns）并实测 HO/LO 波形确认，避免与内部死区叠加导致失真。
- 频率：20kHz 起步；HIN/LIN 高有效，初始占空比 0（输出关闭），先使能 `DRV_+12V_EN` 再开输出。
- 安全：保留 `emergency_stop`（force low）路径；启动前强制低。

### 6.3 SPI（KTH7823）
- mode 3（CPOL=1/CPHA=1），SCK ≤10MHz，16 位帧。
- 角度读取：MOSI 发送任意（建议全 0），MISO 返回 16 位角度（重叠结构：上一帧响应随下一帧返回）；两次通信间隔 >150ns。
- 寄存器：读 `01+ADR+0`、写 `10+ADR+WRD`，写后需 ≥20ms 等待 MTP 烧写。
- 板级已有 10K 上拉（CS/MOSI/SCLK），无需软件配置。
- 数据格式：16bit 原码，无 CRC（已确认）。

### 6.4 YAML 参数管线（方案 B）
```
config/motor.yaml                      # 电机物理参数（KV70 默认）
scripts/gen_motor_params.py            # PyYAML（环境已具备 6.0.3）
        │  CMake configure 期执行（CMAKE_CONFIGURE_DEPENDS 挂 yaml）
        ▼
${CMAKE_BINARY_DIR}/generated/motor_params_generated.h
        │
App/Control/motor_params.h/.c          # 类型 + 默认值出口
```
- API 设计：`const motor_params_t *motor_params_default(void);` 与 `void motor_params_load(motor_params_t *out);`
  - 现阶段 `motor_params_load` = 拷贝默认值；
  - 后续在线辨识/自整定时，在 `motor_params_load` 内叠加 Flash 覆盖，YAML 保持为出厂初值。
- KV70 初值（G66-18，星型）：
  - 极对数 10；Rs = 0.316/2 = **0.158Ω**（线值÷2）；Ls = 0.237/2 = **0.1185mH**；
  - I_rated 7A / I_peak(10s) 24.3A；Vbus 48V；rpm_max 3300；J = 2.3e-5 kg·m²；
  - flux 待离线辨识校核（`bldc_offline_param_detection` 或实测 Ke 换算）。

## 7. 不在本阶段范围
- FOC 控制环（Clarke/Park/SVPWM/电流环/速度环/位置环）、无感算法。
- 上位机 CAN 协议、参数在线写入。
- 硬件保护策略实现（见 §8 未决问题）。

## 8. 已确认事实与遗留项

已确认（Kaiser 2026-09-18）：

1. **无硬件 FAULT 输入**：当前硬件设计没有 FAULT 信号。保护策略由软件实现（ADC 阈值/WDOG + 预驱内部保护），后续硬件版本可考虑增加。
2. **编码器数据格式**：16bit 原码（无 CRC）。
3. **CANID**：通过 MCU ADC 读取 DIP 电阻网络电压判断开关状态；实现暂缓。
4. **NTC**：原理图网络名与连接器交叉属笔误，**以连接器丝印为准**——PB08 = NTC1，PB09 = NTC0。
5. **PA09**：15K 电阻当前焊在 5V（上拉，默认使能），定稿改回下拉（默认关闭）；固件在定稿时需从开漏切换为推挽。

遗留项（实现阶段处理）：

- CANID 解码表（SW2 + R74/R75/R76/R78/R79 阻值拓扑细化）。
- 定稿硬件改回下拉后的 PA09 输出模式切换（开漏→推挽）。
- 软件过流保护的阈值与触发路径设计（无硬件 FAULT 前提下）。

## 9. 验证方法

| 项 | 方法 |
|---|---|
| 时钟 | UART 打印 CPU/AHB 频率 = 480/160MHz |
| UART | 上位机 115200 收到自检输出 |
| SPI | 读 KTH7823 角度寄存器，手动转动转子/输出轴观察角度变化 |
| ADC | 各通道读静态值：1.65V 偏置（电流）、VBUS 分压、NTC、CANID 电压 |
| PWM1 | 低母线电压 + 小占空比，示波器看 HO1–3/LO1–3；确认死区 |
| 预驱 | 先不接电机，确认 12V 使能时序与无直通 |
| CAN | 回环 + 总线收发 |

## 10. 首次上电问题记录（2026-09-18）

**现象**：烧录成功，但 LED 常亮（PB01 低电平点亮，说明固件未执行到 pinmux 置高），Ozone 调试无法停在 main。

**根因**：`linkers/gcc/user_linker.ld` 的 MEMORY 区域不是 HPM53M1/HPM5361 的真实内存映射（疑似误取自其它 SoC 模板）：

| 段 | 修复前地址 | 问题 |
|---|---|---|
| ILM / DLM | 256K / 256K | 实际各 128K |
| `.data` / `.bss` | 0x01080000 | **该地址不存在** → 启动拷贝/清零触发总线异常 |
| `.noncacheable` | 0x01100000 | **该地址不存在** |
| `.ahb_sram` | 0xF0300000 | 实际 AHB SRAM 在 0xF0400000 |

**修复**：以 SDK 官方 `soc/HPM5300/HPM5361/toolchains/gcc/flash_xip.ld` 替换（与 SuperCap 工程同构）。
**验证**：`.data`→0x00080300、`.bss`→0x000804C8、`.noncacheable`→0x000808D8、`.ahb_sram`→0xF0400000、`.fast`→ILM，全部落在真实内存；入口 0x80003000。

**遗留**：
- ~~`linkers/iar/user_linker.icf` 与 `linkers/segger/user_linker.icf` 存在同样的错误映射~~ → **已修复**（替换为 SDK 官方 `toolchains/{iar,segger}/flash_xip.icf`）。
- 原理图中 BOOT0/BOOT1 的 10K（R5/R6）标注为 DNP——**实测不影响启动**（2026-09-18 验证，已从 flash 正常启动）。
- `make FLASH_TOOL=jlink flash` 的 loadfile 地址**已修复**：0x8003000 → `0x8000400`（镜像最低 LMA = nor_cfg_option），并抽出 `JLINK_FLASH_ADDR` 变量便于维护。

**修复后实测（2026-09-18）**：Ozone 正常命中 main，RTT 输出 `clock: cpu=480000000 Hz, ahb=160000000 Hz`，程序运行正常。

## 11. LED 不可控问题（硬件设计限制，2026-09-18）

**现象**：固件运行正常（RTT/时钟正常），但状态 LED 不闪烁。

**根因**：LED（D9）接在 **PB01**（封装 pin 43 = **ADCIN14**），而 HPM53M1 的该引脚是**纯模拟端口**：

- datasheet §1.3.11："提供 PA 组最多 20 个 GPIO 功能复用引脚"；§2.5："本产品上，GPIO 都分配在 PA(PA00∼PA29) 组"。
- datasheet §2.2 引脚表：ADCIN14 类型为"MCU 模拟端口"，功能仅 `ADC0_IN14 / ADC1_IN14 / ACMP_CMP1_INN7 / OPA1_OUT`，**无数字/GPIO 功能**；全表无任何 GPIO_B 条目。
- 该连接沿用 HPM5361 设计（HPM5361 上 PB01 为 GPIO），迁移到 HPM53M1 后不可用 → **LED 无法由固件控制**。

**处理**：
- pinmux 保留原配置并加注释说明其无效（对 HPM5361 有效）；
- 增加 RTT 心跳输出（`hb=N led=X`，500ms 一次）用于验证软件循环与引脚回读；
- LED 需在下一版硬件改到 PA 引脚（当前 20 个 PA 已全部占用：UART×2 / BOOT×2 / JTAG×5 / DRV_EN / SPI3×4 / MCAN3×2 / SPI1×4，可评估复用 PA08/JTAG_TRST 或 BOOT 引脚）。

**注意**：PB00（ADC_V_CANID）同为模拟端口——作为 **ADC 输入**使用不受影响（仅数字功能不可用）。

## 12. 计时异常与修复（2026-09-18）

**现象**：加入 `intf_clock_measure_cpu_freq()`（使用 `rdcycle` CSR 0xC00 + MCHTMR 0xE6000000）后，程序执行到该调用即跑飞。

**分析**：
- 该函数是固件中**首个**使用 `cycle`(0xC00) 与 MCHTMR 的代码；`hpm_csr_get_core_cycle()` 读的是**用户态别名 0xC00**（而非 M-mode 的 `mcycle` 0xB00）。
- SuperCap 工程与 IrqProfiler 的周期计数一直使用 `mcycle`(0xB00)，实测正常；SDK 的 `clock_cpu_delay_ms/us` 也走 0xC00 路径。

**处理（防御性最小化）**：
- `intf_clock_delay_ms/us` 改为基于 `mcycle` 的自实现（不再调用 `clock_cpu_delay_ms/us`）；
- 移除 MCHTMR 自检函数；
- 反汇编验证：固件中已无 `rdcycle` 指令、无 MCHTMR 引用，计时全部走 `mcycle`。

**遗留**：`rdcycle`/MCHTMR 在本芯片上的具体异常行为待单独隔离测试（最小工程分别读取两者并观察 mcause），当前固件不依赖它们。

## 13. 复位循环诊断（2026-09-18，进行中）

**现象**：RTT 输出高频重复 `clock: ... / hb=1 led=0 printf_cyc=0 delay_cyc=0`——`hb` 恒为 1 说明每次都是新启动（计数器被复位），即**芯片在反复复位**。

**诊断手段**：新增 `intf_sys`（Interface + Driver）读取 **PPOR RESET_FLAG**（0xF4100000，记录最近一次复位原因，W1C）：
- bit0 欠压 / bit1 温度 / bit4 调试复位 / bit5 JTAG 软复位 / bit16-17 看门狗 0/1 / bit24 PMIC 看门狗 / bit30 JTAG IEEE / bit31 软件复位；
- `app_init()` 开头打印 `boot: reset_flags=0x%08x` 后清除——下一轮 RTT 即可直接看到复位源。

**待确认**：复位源（看门狗 / 调试器 / 欠压 / 软件）。若为看门狗，需定位其使能来源（ROM/OTP）并处理。

**更新（v4）**：
- **`PPOR.RESET_FLAG` 为 write-only 寄存器（SVD 确认）**，CPU 读取该寄存器可能导致总线异常（表现为无输出）——已移除读取；改为只读的 `RESET_STATUS`。
- 加入**跨复位启动计数** `boot: seq=N`（NOLOAD 段变量，复位不清零）：递增 = 芯片复位循环；恒定 = 上位机重读缓冲。
- 原理图核对：**VPMC（pin 22）← +3.3V（C21 10µF），连接正常**；供电链 VIN → buck → +5V(VCC5V) → DLDO → +3.3V →（VPMC / DCDC_IN / VIO_B00）。若 480MHz 负载下 +3.3V 跌破 VPMC 复位阈值（典型 2.6V），将产生欠压复位循环——需实测 +3.3V/+5V 波形确认。

**结案（2026-09-18）**：根因确认为**供电不足**——原使用调试器供电，480MHz 运行时欠压复位；**改用外部电源供电后一切正常**。

**实测数据（外部供电，v4）**：
```
boot: seq=<NOLOAD初值> rst_status=0x00000000
clock: cpu=480000000 Hz, ahb=160000000 Hz
hb=2  printf_cyc=7956  delay_cyc=240000517
hb=3  printf_cyc=5455  delay_cyc=240000215
...
hb=81 printf_cyc=5105  delay_cyc=240000197
```
- `delay_cyc≈240,000,200`（标称 500ms×480MHz=240,000,000）→ **delay 精确，反向证明 CPU 实际运行在 480MHz**；
- `printf_cyc≈5,100`（≈10.6µs）→ printf 无性能问题；
- 循环 ≈2Hz 稳定，无复位。

**由此复核**：§12 记录的 `rdcycle`/MCHTMR"异常"很可能是欠压复位循环的表现，而非 CSR 本身问题（当前实现使用 `mcycle`，工作正常，暂不回溯）。

## 14. CAN（MCAN3）驱动与首次总线测试（2026-09-18）

### 14.1 实现

- 驱动：`drv_mcan.c`（SDK 现代 API：`mcan_get_default_config` / `mcan_get_default_ram_config` / `mcan_set_filter_element` / `mcan_get_protocol_status` / TX FIFO 非阻塞发送 / TX Event FIFO）。
- 本次修正：
  1. `app_can_init()` 自行注册驱动（原实现依赖外部先注册，单独调用必失败）；
  2. `send/receive` 实现真实 `timeout_ms`（mcycle；0=不等待、`UINT32_MAX`=无限、其他=毫秒）；
  3. 驱动自管时钟使能（`clock_add_to_group`，幂等）；时钟源/分频仍归板级（`drv_clock`）；
  4. 新增 `hpm_can_get_clock_freq()` / `app_can_get_clock_hz()` 诊断接口。
- 自检：`app_debug_can.c` —— 内部环回自检（无需外部节点）+ 正常模式 1Hz 发送 0x114 + RX 打印 + 状态行。
- 消息 RAM：`mcan3_msg_buf` 位于 AHB SRAM `0xF0401E00`（MCAN 硬性要求）。

### 14.2 首次硬件测试结果（外部供电）

- ✅ **总线收发正常**：`ret=0`、`tx_err=0`、`rx_err=0`、`bus_off=0`；收到外部帧（ID=0x7FF，来自上位机 CAN 工具）。
- ✅ **CAN 时钟实测 `clk=80000000 Hz`**：PLL1 = 800MHz / 10 = 80MHz，与 EVK 参考配置一致；SDK 据此计算 1Mbps 时序，总线验证正确。
- ⚠️ 环回自检 FAILED；std/ext 过滤器配置 FAILED；`tx_ok=0`。

### 14.3 根因与修复（已定位，待应用）

1. **过滤器配置失败**：`mcan_set_filter_element` 内部要求配置模式（`mcan_require_config_mode`：CCCR.INIT=1 且 CCE=1），控制器运行中调用必然失败。SDK demo 的做法是把过滤器放入 `mcan_init` 的 `all_filters_config`（初始化期内配置）。
   - 修复方案：驱动 `config_filter` 临时进入 INIT+CCE → 配置 → 退出（期间短暂停止总线参与，建议在启动阶段完成过滤器配置）。
   - 附注：过滤器失效时 RX 仍工作，是因为 GFC.ANFS 默认 0（不匹配帧也接收进 RXFIFO0）。
2. **`tx_ok=0`**：`IR.TC`（Transmission Completed）需要 `txbuf_trans_interrupt_mask`（TXBTIE）使能，默认 0 → 无 TC 中断。SDK demo 均设 `~0UL`。
   - 修复方案：init 时若请求 `INTF_CAN_EVENT_TX_COMPLETED`，设置 `sdk_cfg.txbuf_trans_interrupt_mask = ~0UL`。
3. **环回自检 FAILED**：直接原因为第 1 项（自检在过滤器步骤即返回失败）；修复后需复测确认环回收发。

### 14.4 修复验证（复测通过）

- ✅ 环回自检 `OK`（原 FAILED）
- ✅ 过滤器配置成功（不再出现 FAILED）
- ✅ `tx_ok` 逐帧递增（滞后一帧显示属正常：状态行在入队后立即打印，TC 中断在帧完成时才到）
- ✅ 总线持续健康：`tx_err=0 rx_err=0 bus_off=0`，外部帧接收正常（ID=0x7FF）
- 追加：RX 回调增加**原样回显**（对应 UART 自检的回显验证）——上位机发送任意帧应收到同 ID / 同数据的回发帧；回显失败会打印 `[CAN] echo FAILED`。

## 15. USB CDC 虚拟串口（2026-09-18）

### 15.1 硬件事实（datasheet + 原理图核对）

- **HPM53M1 的 USB_DP/USB_DM 为专用引脚**（封装 pin48/49，类型 USB，供电组 VUSB），
  **无 IOMUX/ALT 功能**——不需要、也不能为其配置 pinmux（原 pinmux 工具输出无 USB 项是正确的）。
  这是与 HPM5361（USB 复用 PA24/PA25）的关键差异。
- 原理图：D+/D- → U12（PRTR5V0U2X ESD）→ L5（DLW21SN900SQ2L 共模电感）→ J10（3-pin：GND/D+/D-）。
  **连接器无 VBUS**（板卡外部供电，USB 仅数据）。
- **QFN80 无 USB0_VBUS 检测引脚**（数据手册引脚表无此项，原理图亦未引出）→ PHY 必须使用
  **内部 VBUS**（`usb_phy_using_internal_vbus()`）。
- DP/DM 45Ω 下拉在 `board_init()` 中关闭（`board_disable_usb_phy_dp_dm_pulldown()`），
  `board_init_usb()` 中时钟就绪后再确认一次。

### 15.2 实现（复用 SDK CherryUSB，不重复造轮子）

| 层 | 文件 | 说明 |
|---|---|---|
| 配置 | `config/usb_config.h` | 以 SDK 样例 `samples/cherryusb/config/usb_config.h` 为基线（保持完整 device/host 宏，`usbotg_core.c` 两者都包含）；日志改走 SEGGER RTT；HS 默认，可切 FS |
| Interface | `Interface/intf_usb_cdc.h` + `intf_default.c` | 契约：init / write(超时) / read(非阻塞) / rx_callback / is_dtr；语义对齐 `intf_uart` |
| Driver | `Driver/hpm_impl/drv_usb_cdc.c` | CherryUSB 设备栈 + CDC ACM 类 + HPM 端口；描述符；OUT 端点 ISR → SPSC 环形缓冲；TX 非缓存缓冲 + ZLP 处理；DTR 弱符号覆盖 |
| Board | `board.c` / `board.h` | `board_init_usb()`：USB0 时钟 + PHY（内部 VBUS + 下拉关闭） |
| Platform | `App/Platform/{Inc,Src}/app_usb.*` | 注册驱动 + 初始化 + write/write_str |
| Debug | `App/Debug/{Inc,Src}/app_debug_usb.*` | 自检：DTR 上报 + RX 回显 + 1Hz 状态行 |
| 构建 | `CMakeLists.txt` | `CONFIG_CHERRYUSB/USB_DEVICE/USB_DEVICE_CDC_ACM`；`sdk_inc(config)`；RTT 头文件经 `sdk_inc` 全局可见 |

约束与说明：
- 单次 `write` ≤ 512B；USB 缓冲位于 `.noncacheable.non_init`（D-Cache 已使能，DMA 需非缓存内存）。
- 描述符 VID/PID 暂用 HPMicro（0x34B7/0xFFFF），量产前替换为自有 ID。
- 资源占用：FLASH 105,552 B（10.07%）。

### 15.3 测试方法

1. 烧录后 RTT 应出现 `[USB] self-test: USB0 CDC ACM (J10 D+/D-), HS` 及 USB 栈枚举日志。
2. J10（GND/D+/D-）接 PC USB（板卡仍需外部供电；注意 D+/D- 极性）。
3. PC 出现虚拟串口（产品名 `HPM53M1 VCOM`）；打开后 RTT 打印 `[USB] host port OPENED (DTR=1)`。
4. 终端输入字符 → 原样回显 + RTT `[USB] rx ...`；打开端口后每秒收到 `usb: tick=N rx_total=M`。
5. 若枚举失败：查看 RTT 的 USB 栈日志；可切换全速模式（`config/usb_config.h` 中
   `#define CONFIG_USB_DEVICE_FORCE_FULL_SPEED`）重建复测。

### 15.4 首次硬件测试结果与修正（2026-09-18）

**结果**：
- ✅ **枚举成功，回显正常**（终端输入 `asdfghjkl` 原样返回；RTT `[USB] rx n=9 total=9 last=0x6C ('l')`）——RX/TX 通路均验证通过。
- ⚠️ `[E/usbd_core] descriptor <type:F,index:0> not found!`（`wValue 0x0F00` = **BOS 描述符**请求）：
  CherryUSB 在未提供 `bos_descriptor` 时对 BOS 请求回 STALL——这是 **USB 2.0 设备的标准行为**，
  主机随后正常继续枚举（实测不影响功能）。如需消除日志可后续补充 BOS 描述符（须同时评估 LPM 能力声明，暂缓）。
- ⚠️ 上位机串口工具默认**不置 DTR** → 原实现（banner/周期行以 DTR 为门控）不输出。
  **修正**：banner 改为"写成功前持续重试"，周期状态行不依赖 DTR（写失败静默）；DTR 仅作信息上报。
- `boot: seq=5`：上电以来第 5 次启动（含烧录/复位），`rst_status=0` 无异常复位；
  MCU 复位瞬间虚拟串口掉线属正常 USB 重新枚举行为。

**修改**：
- 设备名（iProduct）→ `HPM53M1_G6618Motor`（与工程同名）
- 序列号 `0001` → `0002`（使 Windows 刷新设备名缓存，否则可能仍显示旧名）
