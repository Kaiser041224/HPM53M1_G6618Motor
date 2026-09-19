# ADC 采样与物理量转换设计（2026-09-19）

> 状态：已实现（cda2617 之后的 ADC 批次）
> 关联：`2026-09-18-m1-board-bringup-design.md` §2.2 / §6.1 / §6.4

## 1. 目标与范围

- M1 板：三相低侧电流 + 母线电压 + 两路 NTC 的采样链与物理量换算。
- 触发源与采样时刻：PWM1 中心对齐输出的低侧导通窗口内，每 PWM 周期一次。
- 本阶段不包含：过流保护策略（WDOG 阈值）、NTC 温度换算（型号未定）、FOC 电流环。

## 2. 硬件事实（原理图核实）

| 项 | 值 | 来源 |
|---|---|---|
| 相电流 | 低侧 2mΩ 三电阻 + TPA6584Q 差分放大：输入 2K / 反馈 15K → **增益 7.5**，偏置 1.65V（VREF_3.3V 经 15K/15K 分压 + U9A 缓冲） | Analog Signal Processing 页 |
| 母线电压 | VBUS → 15K×4 + 10K + 3.3K 分压（73.3K/3.3K，满量程 73.3V）→ U2F 内部运放 B 缓冲 → PB14 | 同上 |
| NTC | 10K 1% 上拉至 3.3V_A，NTC 在连接器上（外部件，型号待定） | 同上 |
| 基准 | U7 TPR3533 独立 3.3V LDO（VREF_3.3V = 模拟域基准） | §2.2 |

**勘误记录**：原理图中 I_V 通道（U9D）输入电阻标注为 15K（其余两相同为 2K）——已确认是**标注错误**，
实物已更换为 2K，三路增益统一 7.5。后续不要按原理图标注误判。

## 3. 触发链设计

### 3.1 路径

```
PWM1 CMP10（cmp = 计数谷底 + delay_ns）
  → CHCFG[10]（cmp_start = cmp_end = 10，匹配处窄脉冲）
  → PWM1_CH10REF
  → TRGM0 PTRGI0A（TRGM_TRGOCFG_ADCX_PTRGI0A，一路同时启动 ADC0/ADC1 的 TRG0A）
  → ADC0 PMT / ADC1 PMT
```

### 3.2 关键事实（SDK 核实，2026-09-19）

1. **不存在 `PWM1_CMP0 → TRGM` 直接枚举**；必须经 PWM 输出通道 CH8~15 的 REF 信号进入 TRGM。
2. **HPM5300 PWM 为单向递增计数器**（0 → reload → 回卷），寄存器无 up-down/方向字段；
   "中心对齐"由 `cmp_begin/cmp_end` 围绕 `reload/2` 对称摆放合成，**周期 = (reload+1) 个时钟**。
   → 每个比较器**每周期只匹配 1 次**，触发干净，无需方向过滤。
3. 低侧导通窗口对称分布在**计数谷底**两侧，半宽 = `(1−占空比) × T/2`。
   触发点 = 谷底 + `delay_ns`（默认 500ns）。
4. `PTRGIxY` 同时驱动 ADC0 与 ADC1 的 TRGxY（SDK QEIV2 示例证实）。

### 3.3 触发接口（`intf_hrpwm`）

```c
int intf_hrpwm_config_trigger_cmp(inst, cmp_index, delay_ns);  /* 初始化 */
int intf_hrpwm_set_trigger_cmp_delay(inst, cmp_index, delay_ns); /* 运行期单次写（调试扫描） */
```
`tick = f_pwm_clk × delay_ns / 1e9`，与 PWM 频率解耦（改频率不影响 ns 延时）。

## 4. 采样配置

**架构（2026-09-19 定稿）**：

| ADC | 通道 | 机制 | 速率 |
|---|---|---|---|
| ADC0 | I_U(IN3/PB11)、I_V(IN4/PB12)、I_W(IN2/PB10) | PMT 队列 `[I_W副本, I_U, I_V, I_W]` | 25kHz |
| ADC1 | V_VBUS(IN6/PB14)、NTC0(IN11/PB08)、NTC1(IN1/PB09)、V_CANID(IN15/PB00) | **周期转换模式（硬件自主定时）** | **~1kHz** |

**设计依据**：电流环需要与 PWM 同步的实时采样（PMT）；VBUS/NTC/CANID 无速率要求
（母线动态 ≪1kHz，FOC 关心的是**周期平均电压**，1kHz + 滤波优于 25kHz 同步采样的纹波混叠）。
该划分省去 ADC1 的 PMT 与 ISR（≈7% CPU），为 FOC 留出余量。

| 项 | 值 |
|---|---|
| ADC0 PMT 队列 | `[I_W(副本), I_U(IN3), I_V(IN4), I_W(IN2)]`（首槽=队尾副本，见 §9.1） |
| ADC1 周期模式 | `PRESCALE=8, PRD=156` → 39936 ADC 时钟 = **1001.6 Hz**；CPU 只读 `PRD_RESULT`（不触发转换） |
| ADC1 dummy | IN0（未键合、浮空）作首转换吸收 S/H 残留（周期模式按通道号升序转换） |
| 触发通道 | 仅 ADC0 使用 `ADC16_CONFIG_TRG0A`（PTRGI0A） |
| 分辨率 / 采样窗口 | 16-bit / sample_cycle = 10（可配） |
| ADC 时钟 | AHB 160MHz ÷ 4 = **40MHz**（25ns/tick；驱动强制 ≤50MHz） |
| 单通道转换 | 21+10 = 31 tick = **775ns**；三路 = 2.33µs |
| 第 3 路采样窗口结束 | 谷底 + 500ns + 2×775ns + 250ns ≈ **2.3µs** |
| 允许最高占空比 | **≈ 0.885**（`(1−d)×20µs ≥ 2.3µs`）；更高调制区由 FOC 两相重构处理 |
| 启动丢弃 | 前 8 帧（驱动 `ADC_PMT_STARTUP_DISCARD`） |
| DMA 缓冲 | 每实例 48 字（12 触发 × 4 槽），驱动内部 fast RAM（DLM，非缓存）分配 |

## 5. 软件架构

```
App/Logic/app_logic.c            init 步骤 10/11 + 25kHz 主循环 process
App/Debug/app_debug_adc.*        配置打印（init）+ 通道表（d）+ 诊断（p）
App/Debug/app_debug_cmd.c        d / p / k / n 命令
App/Platform/app_adc.*           通道表 + PMT 配置 + 触发链 + raw 缓存 + 序号
App/Platform/app_analog_signal.* 换算 + 标定 + 滤波 + 快照
Interface/intf_adc.h             契约（本批次清理：删 set_vref/calibrate/deinit/死字段）
Driver/hpm_impl/drv_adc.c        PMT/DMA/丢弃/校验/诊断（本批次：内部 DMA 缓冲 + 通道校验）
Interface/intf_hrpwm.h + drv_hrpwm.c  触发比较器 ns 化
```

### 5.1 `app_adc` 接口

```c
typedef enum { ADC_CH_I_U, ADC_CH_I_V, ADC_CH_I_W, ADC_CH_V_VBUS, ADC_CH_NTC0, ADC_CH_NTC1, ADC_CH_COUNT } adc_channel_t;
typedef struct { uint32_t trigger_delay_ns; uint16_t sample_cycle; uint8_t resolution; } app_adc_cfg_t;

void     app_adc_init(const app_adc_cfg_t *cfg);   /* NULL = 默认；含触发链配置并启动 PWM1 计数 */
const app_adc_cfg_t *app_adc_get_config(void);
bool     app_adc_get_raw(adc_channel_t ch, uint16_t *raw);
uint32_t app_adc_get_sequence(void);               /* 电流帧序号 */
bool     app_adc_is_valid(void);
float    app_adc_code_to_volts(uint16_t code);
int      app_adc_set_trigger_delay_ns(uint32_t delay_ns);
```

- `app_adc_init` 末尾调用 `intf_hrpwm_start_counter_only(PWM1)`：**采样链独立于逆变桥使能**，
  桥臂关闭时 PMT 仍持续采样（引脚输出保持关闭）。
- ISR 回调写 raw 缓存（先写值、后加序号），主循环读取。

### 5.2 `app_analog_signal` 接口

```c
typedef struct { float i_u_a, i_v_a, i_w_a, v_bus_v, r_ntc0_ohm, r_ntc1_ohm; } app_analog_values_t;

void  app_analog_signal_init(void);
void  app_analog_signal_process(void);                        /* 25kHz */
bool  app_analog_signal_read_all(app_analog_values_t *values);
float app_analog_signal_read(adc_channel_t ch);               /* 未就绪返回 NAN */
bool  app_analog_signal_read_raw(adc_channel_t ch, uint16_t *raw);
int   app_analog_signal_calibrate_offsets(void);
```

## 6. 物理量换算

```
V_adc [V] = raw × 3.3 / (2^res − 1)
I_x   [A] = (V_adc − V_zero) × 66.6667      （V_zero = 零点标定值，默认 1.65V）
V_bus [V] = V_adc × 22.2121                 （73.3K/3.3K）
R_ntc [Ω] = 10000 × V_adc / (3.3 − V_adc)   （V_adc ≥ 3.299V → 1MΩ 上限）
```

电流链路为比例式：1.65V 偏置与 ADC 基准同源，3.3V 电源漂移不影响精度。

## 7. 零点标定

- `calibrate_offsets()`：等待链路就绪 → 阻塞采集 256 帧（~10ms）→ 三路电流 raw 均值 → 零点电压。
- 保护：等待/采集共用 50ms 超时；单帧偏离中值超过 **5% FS** 判定为有电流，拒绝标定。
- 默认（未标定）：1.65V；`n` 命令可重标（须无电流）。

## 8. 滤波

| 通道 | 默认 | 说明 |
|---|---|---|
| I_U / I_V / I_W | NONE | FOC 电流环保真（无相位滞后） |
| V_VBUS | LPF 100Hz | 直流慢变量 |
| NTC0 / NTC1 | MA 32 | 极慢变量 |

## 9. 调试与验证

| 命令 | 功能 |
|---|---|
| `d` | 全通道表：raw / mV / 物理量 + 帧序号 |
| `p` | 诊断：ISR/PMT 计数、非法帧细分、ISR 周期/占用率、实测 PMT 速率（应 ≈25000 Hz） |
| `k` | 触发延时预设循环（100/250/500/1000/2000 ns） |
| `n` | 电流零点标定 |

**Ozone 观测变量**（`.noncacheable.bss`，25kHz 刷新；与编码器 `g_enc_*` 同风格）：

| 变量 | 含义 |
|---|---|
| `g_adc_i_u_a` / `g_adc_i_v_a` / `g_adc_i_w_a` | 三路相电流 [A] |
| `g_adc_v_bus_v` | 母线电压 [V] |
| `g_adc_r_ntc0_ohm` / `g_adc_r_ntc1_ohm` | NTC 电阻 [Ω] |
| `g_adc_raw[6]` | 原始码（顺序 I_U, I_V, I_W, V_VBUS, NTC0, NTC1） |
| `g_adc_sequence` | 电流帧序号 |

**预期值**：
- 静态（桥臂零矢量/未旋转）：I_U/I_V/I_W ≈ 0 A（±50mA，标定后）；V_bus = 实际母线（±2%）；NTC 未安装 → 高阻（≈1MΩ 或与悬空分压对应）。
- 开环旋转（m=3~10%）：三路电流呈正弦，幅值 ≈ `m·Vbus/2/0.158Ω`（24V、m=3% → ~2.3A）。

## 9.1 实测确认的硬件行为：PMT 队列首槽污染（2026-09-19）

**现象**：PMT 队列的**首槽**在竞态下会得到"上一帧最后一个通道"的采样值，而元数据仍标注为队列首通道。

**证据（三次独立观测）**：

| 队列配置 | slot0 应为 | 实测 slot0 | 后果 |
|---|---|---|---|
| `[ch3,ch4,ch2]`（3槽） | ch3=1.65V | ch2 的值 | app 的 I_U 实为 I_W → "U/W 波形重合" |
| `[ch6,ch1,ch11]`（3槽） | ch6=1.08V | ch11 的值（3.29V） | "V_VBUS 读数 73V（满量程）" |
| `[ch3,ch4,ch2,ch6]`（4槽） | ch3=1.65V | ch6 的值（1.07V） | 交叉验证复现 |
| `[ch6,ch1,ch11,ch3]`（对照） | — | 四槽全部正确 | 证明为**竞态**（时好时坏） |

**机理（工程推断，非寄存器手册级结论）**：帧间约 36µs 空闲后，触发启动的**首个转换**其 S/H 采集与输入通道 mux 切换之间的时序余量不足——首转换仍持有上一帧最后通道的电荷，而硬件按队列首通道标注该槽。

**修复（双份队尾方案，不浪费通道）**：队列 = `[D, A, B, D]`，D 同时位于首槽与队尾：
- 发生污染时：slot0 = D 的**上一帧值**（元数据 D，自洽且有效）
- 未污染时：slot0 = D 的本帧值
- slot1..3 = A、B、D 的**本帧新鲜值**（主用）
- slot0 作为 D 的高调制备用副本（其采样点最早，始终落在低侧导通窗口内）

本板队列：
```
ADC0: [I_W, I_U, I_V, I_W]        → 读 slot1=I_U, slot2=I_V, slot3=I_W
ADC1: [NTC1, V_VBUS, NTC0, NTC1]  → 读 slot1=V_VBUS, slot2=NTC0, slot3=NTC1
```
8 槽位承载 6 个不同通道 ✓（对比 dummy 方案只能承载 4 个 ✗）。

**注意**：这不是"启动丢弃"（那由驱动的 8 帧启动丢弃负责），而是**每帧**都存在的硬件行为——首槽必须安排为队尾通道的副本；若改成无副本的队列，该 bug 会立即复现（本项目已实测复现过一次）。
**手册核查（2026-09-19）**：用户手册 §53（PMT 数据格式/cycle-bit 协议）与 §20（通道映射）均与实现一致；该首槽行为未收录于用户手册与勘误表（E00023~E00048）。

## 9.2 慢通道采样：ADC1 序列转换 @1kHz（GPTMR0 → TRGM → STRGI）（2026-09-19 定稿）

**背景**：PMT 两个队列的 8 个槽位承载 6 个通道后，CANID（PB00 = ADC1_IN15）无法再进队列。

**最终方案**：ADC1 全部慢通道改用**序列转换模式**，由
**GPTMR0 CH2（1kHz 方波）→ TRGM0（输入 `GPTMR0_OUT2`）→ `ADC1_STRGI`** 硬件触发，
与 25kHz PWM 触发完全解耦（不再占用 PTRGI0A/抢占队列）：

- 序列 `[CANID, V_VBUS, NTC0, NTC1, CANID]`（首项=队尾副本，首转换 S/H 残留自吸收），
  5 项/轮 × ≈1.15µs ≈ 5.8µs/ms（0.6% ADC 占空）；
- 结果由硬件同步到 `PRD_RESULTx`（**实测确认：任意转换模式都会更新 PRD_RESULTx**，
  手册 §53.2.4），1kHz 慢任务纯寄存器读取——无触发副作用、无读冲突；
- 无中断、无 DMA、无 ISR（`seq_cb = NULL` → 驱动不使能中断/PLIC）。

**演进过程与实测教训（同日）**：
1. 读取模式（BUS_RESULT 轮询）在 ADC1 **多通道**下不可用——四通道读数返回同一
   "共享输出"（在 1.07V/3.3V/0 间漂移），寄存器配置经 dump 核实无误；手册 §53.4.4
   明确"AD 转换进行中读其它通道结果寄存器不会触发转换、只返回旧结果并置 read_cflict"。
2. 周期模式实测不运行（PRD_RESULT 冻结）。
3. **过渡方案 PMT+DMA（TRG0A @25kHz）实测可用**（队列 `[NTC1,V,NTC0,NTC1]`、无 ISR、
   1kHz 轮询 DMA 缓冲，V_VBUS/NTC0/NTC1 硬件验证通过）；后按"慢通道与 25kHz 触发
   解耦 + 硬件定时 1kHz"需求升级为当前序列方案。

| 项 | 说明 |
|---|---|
| 触发 | GPTMR0 CH2 PWM 1kHz/50% → TRGM0 输入 `GPTMR0_OUT2` → 输出 `ADC1_STRGI` |
| 驱动 | SEQ 模式 `seq_cb==NULL` 时不使能中断/PLIC（纯轮询用法） |
| 驱动 | `adc_read()` SEQ 分支读 `PRD_RESULTx`（纯寄存器，无副作用） |
| 驱动 | oneshot 模式不再强制 `SEL_SYNC_AHB=1`（手册要求 ADC 时钟=总线时钟才可置 1；本板为 AHB/4） |
| app_adc | `app_adc_slow_process()`（1kHz）：读 4 个慢通道（含 CANID）并更新缓存 |
| 有效性 | `app_adc_is_valid()` 仅统计 PMT 电流通道（慢通道不参与） |
| 调试 | `d` 命令显示全部 7 通道；`p` 增印 ADC1 INT_STS / SEQ_CFG0 / PRD_RESULT[6,11,1,15] |
| 验证 | DIP 拨动实测：V_CANID 读数随开关变化 ✓（2026-09-19）；电压→4bit ID 解码表待原理图拓扑确认 |

## 9.3 GPTMR 慢速触发链与驱动修复（2026-09-19）

**驱动缺陷（已修复）**：`drv_gptmr.c` 的 `gptmr_drv_init()` 中 `gptmr_apply_duty()`
在 `gptmr_state[ch].reload = reload` **之前**执行，导致 PWM 初始化时 `reload == 0` →
`CMP0 = CMP1 = 0`。手册 §43.2.2 明确"**CMP0 与 CMP1 相等时，输出无变化**"→ GPTMR0 CH2
输出恒定不翻转 → 1kHz 触发链静默失效（`SEQ_CFG0` 配置正确但 `INT_STS=0`、序列从不运行）。

**修复**：把 `gptmr_state[ch].reload = reload;` 移至 `switch (cfg->mode)` 之前。
**该缺陷影响模板驱动的所有 PWM/PWM_TIMER 用法，建议同步回环境仓库模板。**

**修复后实测**：GPTMR0 CH2 输出 1kHz 方波（`RLD=0x18FF0`、`CMP0/CMP1` 正确），
ADC1 序列按 1kHz 运行（`INT_STS` = SEQ_CMPT|SEQ_CVC = `0x01800000`），
4 个慢通道 `PRD_RESULT` 全部刷新；V_VBUS 稳定 23.7~23.9V（旋转前后）。

## 9.4 ADC0 触发风暴观察（2026-09-19，间歇、暂不复现）

**现象**：旋转启动瞬间，ADC0 PMT 出现触发倍增（ISR 计数暴涨）：
- 观测记录：116kHz / 113kHz / 228kHz / 762kHz（各一次），持续 ~0.3~0.6s 后自愈回 25kHz；
- 风暴期间 `isr_cpu` 显示 >100%（**根因：`isr_total_cycles` 32 位截断导致差值回绕——
  已修为 64 位**，非真实占用）；
- 风暴期间 `CONFIG0`、`CONV_CFG1` 保持正常（队列与时钟未被破坏）。

**排查结论（未完全定位）**：
- 稳态寄存器全部正确：`RLD=0x18FF0`（6399）、`CMP10=0x500`（80 ticks = 500ns）、
  `TRGM0 TRGOCFG[34]=0x23A`（PWM1_CH10REF + 上升沿脉冲）；
- 触发链物理上限：4 槽队列 ≈4.6µs → ≤217kHz，而观测达 762kHz → 提示
  "TRIG_CMPT 标志重入/未清"类机制（ISR 背靠背重入），而非真实帧倍增；
- **ADC1 稳定运行 SEQ 后不再复现**（连续 4+ 次旋转启动零复现）——初步推测与
  ADC1 相关（历史风暴均出现在 ADC1 占用/异常时期）；**未证实，保留观察**；
- 取证代码（ISR 内风暴锁存）已完成使命并移除。

**若复现**：优先检查 `INT_STS` 的 TRIG_HW_CFLCT（bit29）与 ISR 间隔；必要时把
PWM1_CH10REF 经 TRGM 引出到引脚用示波器抓取。

## 10. 已知风险与后续项

1. **触发延时 500ns 为初值**：用 `k` 扫描确认采样点位于低侧窗口内（读数稳定性/线性度最优）。
2. **ADC0 旋转启动瞬态风暴**：间歇、自愈；ADC1 稳定后暂不复现（见 §9.4）——FOC 接入前需确认。
3. **停机电流噪声 ±0.7A（±230 counts）**：待旋转测试后评估是否需要在 app_analog_signal 增加轻量滤波。
4. **DMA 缓冲位于 DLM**：若 ADC 内部 DMA 无法写 DLM（预期可写，SDK `core_local_mem_to_sys_address` 在本 SoC 为恒等映射），
   表现为 `p` 中 `inv` 计数增长、回调不执行 → 备选方案：换 AHB SRAM 非缓存段。
5. **16-bit 下 d_max≈0.885**：FOC 阶段处理（两相重构/最小窗口保护）。
6. **三路电流分时采样间隔**：单通道 ≈1.15µs、三路首末 ≈2.3µs → 550Hz 电频率下 <0.5°。
7. **NTC 温度换算**：待实物安装与型号确认后补参数（B 值/查表）。
8. **V_CANID 解码表**：DIP 拨动验证采样链正常（2026-09-19）；电压 → 4bit ID 映射待原理图拓扑确认。
9. **过流保护**：WDOG 阈值策略未定（保护阶段设计）。
10. **主循环时序余量**：旋转期间实测 25kHz 循环丢拍 ~4%（late≈936/s）——与 ADC 无关（独立 ISR 采样），
    但 FOC 电流环接入前需先修余量（慢任务/打印路径定位后优化）。
