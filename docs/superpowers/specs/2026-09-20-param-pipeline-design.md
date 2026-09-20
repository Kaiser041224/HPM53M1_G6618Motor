# 参数管线设计：YAML → 工厂默认参数（三域：机械 / 硬件 / 软件）

- 日期：2026-09-20
- 状态：设计已确认（Kaiser），待实施
- 关系：**本设计取代** `2026-09-18-m1-board-bringup-design.md` §6.4（单文件 `motor.yaml` + 宏方案）

---

## 1. 背景与目标

### 1.1 背景

当前可配置参数散落在 4 处：

| 位置 | 内容 | 问题 |
| :--- | :--- | :--- |
| `app_fault.h` | 7 项保护阈值宏 | 与电机/硬件参数的关系不可见（72.9 = 3×24.3A 只存在于注释） |
| `app_3phase_inverter.h` / `app_adc.h` | 逆变频率/死区、触发时序默认值宏 | 换硬件需翻代码 |
| `app_analog_signal.c/.h` | 换算常数宏（66.6667 / 22.2121 / …） | 与原理图参数的推导关系不可见 |
| `app_debug_*.c` | V/F 调试默认值 | 调试用途，保留 |

### 1.2 目标

1. **YAML 为出厂默认初值的唯一来源**（机械 / 硬件 / 软件三域，三文件）；
2. 构建期由生成器求值 + 校验 → 生成**类型化 C 参数结构体**；
3. 为将来**在线自校准 / 整定 → flash 保存**预留覆盖点（`*_load()` 是唯一改动点）；
4. YAML 支持**表达式与跨文件符号引用**，让参数之间的物理关系显式化。

### 1.3 非目标（本轮不做）

- 运行期解析 YAML（MCU 上无解析器；全部构建期完成）；
- 引脚/通道映射入 YAML（属 Board 层，改它等于改硬件）；
- flash 覆盖的实际实现（v2；本轮仅预留 API）；
- NTC 温度换算、CANID 解码的**逻辑**（硬件未定；本轮仅占位值）；
- FOC 参数的**消费**（FOC 未实现；本轮生成 + 校验，不接线）。

---

## 2. 总体架构

```text
config/motor.yaml ─────┐
config/hardware.yaml ──┼─► scripts/gen_params.py ──► build/generated/params_generated.{h,c}
config/software.yaml ──┘        （构建期：求值/校验）      build/generated/params_report.txt
                                                              │ const 工厂实例
                                                              ▼
   ┌────────────────────────────────────────────────────────────────┐
   │ App/Control/app_motor_params.{h,c}   app_motor_params_t        │ ← FOC（后续）
   │ App/Platform/app_hw_params.{h,c}     app_hw_params_t           │ ← app_analog_signal / app_adc / app_3phase_inverter
   │ App/Platform/app_sw_params.{h,c}     app_sw_params_t           │ ← app_fault / app_can / FOC（后续）
   └────────────────────────────────────────────────────────────────┘
```

### 2.1 文件布局

| 文件 | 提交 | 说明 |
| :--- | :--- | :--- |
| `config/motor.yaml` | ✓ | 机械参数：绕组/转子/编码器关系 |
| `config/hardware.yaml` | ✓ | 硬件参数：采样电路/ADC 时序/功率级 |
| `config/software.yaml` | ✓ | 软件参数：保护阈值/CAN/控制（FOC 预留） |
| `scripts/gen_params.py` | ✓ | 生成器（PyYAML；含字段模式表与校验规则） |
| `build/generated/params_generated.h` | ✗ | 生成物：`extern const` 三域工厂实例声明 |
| `build/generated/params_generated.c` | ✗ | 生成物：工厂实例定义（计算后字面量） |
| `build/generated/params_report.txt` | ✗ | 生成物：全参数 = 值/表达式 对照表（审查用） |
| `App/Control/app_motor_params.{h,c}` | ✓ | 类型 + 访问器（手写） |
| `App/Platform/app_hw_params.{h,c}` | ✓ | 类型 + 访问器（手写） |
| `App/Platform/app_sw_params.{h,c}` | ✓ | 类型 + 访问器（手写） |

### 2.2 分层与依赖

- `app_motor_params` → **App/Control**（消费者是 FOC；Platform 不需要机械参数）；
- `app_hw_params` / `app_sw_params` → **App/Platform**（Platform 模块直接消费；Control/Debug 向下可达）；
- 生成头 `params_generated.h` **只被三个 params 模块的 .c 引用**，不对外扩散；
- 三个 params 模块无 init 依赖（纯常量数据）；将来 flash 叠加在 `*_load()` 内检测 `app_param_is_ready()`。

### 2.3 消费方式

- 模块在 `init(NULL)` / 无 cfg 调用时**内部调用 `*_load()`** 取默认值，保持现有"NULL = 默认"语义（调用点零改动）；
- 现有 `cfg_t` 运行时覆盖路径不变（如 `app_fault_init(&custom_cfg)`）；
- 参数值在模块 init 时拷入模块静态状态（后续 process 热路径不重复调用 load）。

---

## 3. YAML 设计规范

### 3.1 通用约定

1. **键名**：`snake_case` + 单位后缀：

   | 后缀 | 含义 | 后缀 | 含义 |
   | :--- | :--- | :--- | :--- |
   | `_a` | 安培 | `_hz` | 赫兹 |
   | `_v` | 伏特 | `_ns` / `_ms` / `_s` | 时间 |
   | `_ohm` | 欧姆 | `_bits` | 位数 |
   | `_h` | 亨利 | `_kgm2` | kg·m² |
   | `_nm` | 牛·米 | `_rpm` | 转/分 |
   | `_k` | 开尔文 | `_ticks` / `_samples` | 计数 |

2. **值**：数字字面量（`0.158`、`1.185e-4`）或**引号字符串表达式**（见 §3.2）；布尔/文本按字段类型；
3. **注释**：每个字段带单位与来源/依据说明；
4. **派生量用表达式声明，不写死结果**（如 `a_per_volt: "1 / (shunt_ohm * amp_gain)"`）；
5. **关系明确用表达式，独立设计选择用带注释的数值**（如 `vbus_uv_v: 9.0  # 24V 系统门限`）；
6. **同义复用**：跨文件引用同义变量（如 `oc_trip_a: "3 * motor.i_peak_10s_a"`）；
7. 文件根 = 域；根级键为域的字段/节；节内字段通过 `节.字段` 引用。

### 3.2 表达式与符号引用

**语法**：`+ - * / **`、括号；函数白名单 `sqrt / sin / cos / atan2 / min / max / abs / pi`。

**引用规则**：

| 场景 | 写法 | 示例 |
| :--- | :--- | :--- |
| 同文件、叶子键 | `leaf` | `rotor_ring_teeth / output_pinion_teeth` |
| 同文件、跨节 | `section.leaf` | `encoder.output_pinion_teeth` |
| 跨文件 | `file.path` | `3 * motor.i_peak_10s_a` |

- 叶子键名允许重复（如 `current_loop.kp` 与 `speed_loop.kp`）；**bare 引用**仅在文件内唯一时可用，歧义时报错并列出候选路径（完整路径引用始终可用）；
- 求值：按依赖拓扑排序；**环引用 / 未定义符号 / 除零 / 类型不符 → 构建失败**，
  错误消息格式为 `文件:行:键: 原因`（YAML 键可定位到行时带行号，缺行号时退回 `文件:键: 原因`）；
- 类型：整数字段要求结果为整值（误差 < 1e-9），浮点字段接受整数/浮点；结果必须为有限值。

### 3.3 构建期校验（硬失败）

**模式校验**：必填键缺失、未知键（防拼写错误）、类型不符、范围越界。

**跨域一致性**（生成器内置规则，按需扩展）：

| # | 规则 | 依据 |
| :--- | :--- | :--- |
| 1 | `pole_pairs ≥ 1`（整数） | 电角度换算 |
| 2 | `vbus_uv_v < vbus_ov_v ≤ motor.vbus_nom_v` | 保护逻辑（阈值次序 + 不超电机额定电压） |
| 3 | `oc_trip_a ≥ motor.i_rated_a` | 额定不误报 |
| 4 | `oc_trip_a ≤ current_sense.bias_v × current_sense.a_per_volt` | 不超采样链路满量程（±110A） |
| 5 | `sample_cycle ≥ 10` | SDK 最小值（多工程复现过数据异常） |
| 6 | `deadtime_ns < 0.5 × 10⁹ / inverter.pwm_freq_hz` | 死区 < 半开关周期；> 5% 周期时告警 |
| 7 | `encoder.resolution_bits ∈ [1, 32]`，齿数 ≥ 1 | 角度解算 |
| 8 | 物理量 > 0（`rs_ohm` / `ls_h` / `inertia_kgm2` / `rpm_max` / `baudrate` 等） | 物理意义 |

---

## 4. YAML 定稿

### 4.1 `config/motor.yaml`

```yaml
# ============================================================================
# motor.yaml — 电机机械/电磁参数（出厂默认初值）
#
# 用途：FOC 算法（后续）与保护阈值的物理依据；本文件为唯一来源。
# 说明：机械零点等标定值不进本文件（存 flash，app_param key "ENCD"）。
# 表达式：引号字符串按计算式处理，构建期求值（跨文件引用 motor./hardware./software.）。
# ============================================================================

name: "G66-18 KV70"        # 仅文档（不生成 C 字段）
winding: star              # 仅文档：star | delta
pole_pairs: 10             # 极对数（24N20P → 20 极 / 2）；电角度 = 机械角 × 10
rs_ohm: 0.158              # 相电阻 [Ω]（线值 0.316 ÷ 2）
ls_h: 1.185e-4             # 相电感 [H]（线值 0.237mH ÷ 2）
ke_vs_per_rad: 0.125       # 反电动势系数 [V·s/rad]（KV70 手册值；待离线辨识校核）
kt_nm_per_a: 0.117         # 转矩系数 [N·m/A]（手册值；SI 下理想 PMSM 应与 Ke 相等，差异待校核）
i_rated_a: 7.0             # 额定电流 [A]（RMS，105°C）
i_peak_10s_a: 24.3         # 峰值电流 10s [A]（RMS）
i_peak_2s_a: 48.6          # 峰值电流 2s [A]（RMS）
vbus_nom_v: 48.0           # 母线额定电压 [V]（电机规格 12~48；当前测试系统母线 24V，保护门限见 software.yaml）
rpm_max: 3300              # 最高转速 [rpm]
inertia_kgm2: 2.3e-5       # 转动惯量 [kg·m²]
torque_rated_nm: 1.16      # 额定转矩 [N·m]（手册值；与 Kt×I 差异待校核）
torque_peak_10s_nm: 2.8    # 峰值转矩 10s [N·m]（手册值；与 Kt×I 差异待校核）

encoder:
  model: "KTH7823"              # 仅文档
  resolution_bits: 16           # 单圈绝对分辨率 [bit]
  rotor_ring_teeth: 49          # 转子轴外齿圈齿数（两路小齿轮共用）
  rotor_pinion_teeth: 49        # 转子编码器小齿轮齿数（49:49 → 与转子 1:1）
  output_pinion_teeth: 50       # 出轴编码器小齿轮齿数（49:50）
  rotor_ratio: "rotor_ring_teeth / rotor_pinion_teeth"     # 转子编码器转角/转子转角 → 1.0
  output_ratio: "rotor_ring_teeth / output_pinion_teeth"   # 出轴编码器转角/转子转角 → 0.98（实测确认）
  # reducer_ratio: <待确认>    # 减速器出轴/转子减速比（多圈解算用，待机械确认后加入）
```

### 4.2 `config/hardware.yaml`

```yaml
# ============================================================================
# hardware.yaml — 硬件电路参数（出厂默认初值）
#
# 用途：采样链路换算、ADC 触发时序、功率级配置的唯一来源。
# 说明：引脚/通道映射属 Board 层，不进本文件。
# 表达式：引号字符串按计算式处理，构建期求值（跨文件引用 motor./hardware./software.）。
# 占位项：NTC 型号未定、CANID 解码未实现（见各节注释，待硬件确认后修正）。
# ============================================================================

board:
  name: "HPM53M1_G6618Motor"   # 仅文档

current_sense:
  shunt_ohm: 0.002                        # 采样电阻 [Ω]
  amp_gain: 7.5                           # 运放增益（TPA6584Q）
  a_per_volt: "1 / (shunt_ohm * amp_gain)"  # 电流标度 [A/V] → 66.6667
  bias_v: 1.65                            # 零电流偏置 [V]（= vref/2；vref 固定 3.3V 由驱动侧提供，不进 YAML）

vbus_sense:
  divider_high_ohm: 70000                 # 分压上臂 [Ω]（15K×4 + 10K = 70K）
  divider_low_ohm: 3300                   # 分压下臂 [Ω]（3.3K；与上臂合计 73.3K）
  v_per_volt: "(divider_high_ohm + divider_low_ohm) / divider_low_ohm"  # 母线标度 [V/V] → 22.2121

ntc:
  pullup_ohm: 10000                       # 板上上拉 [Ω]
  r25_ohm: 10000                          # 占位：NTC 型号未定（按典型 10K 填写，待选型修正）
  b_value_k: 3950                         # 占位：B 常数 [K]（典型 3950K，待选型修正）
  max_ohm: 1000000.0                      # 开路/超量程替代值 [Ω]

canid_dip:
  levels: 16                              # 占位：4-bit 拨码档数；解码逻辑未实现（节点号映射待硬件确认）

adc:
  sample_cycle: 25                        # 采样窗口 [ADC 时钟数]
  trigger_delay_ns: 500                   # 谷底后触发延时 [ns]

inverter:
  pwm_freq_hz: 25000                      # 开关频率 [Hz]
  deadtime_ns: 50                         # HPM 侧死区 [ns]（预驱自带 50~250ns）
```

### 4.3 `config/software.yaml`

```yaml
# ============================================================================
# software.yaml — 软件参数（出厂默认初值）
#
# 用途：保护阈值、通信、控制（FOC 预留）参数的唯一来源。
# 说明：YAML 仅为出厂初值；将来在线辨识/整定结果经 flash 覆盖（设计文档 §7）。
# 表达式：引号字符串按计算式处理，构建期求值（跨文件引用 motor./hardware./software.）。
# ============================================================================

can:
  baudrate: 1000000          # CAN 波特率 [bps]
  node_id_default: 1         # 占位：DIP 解码未实现时的回退节点号（与 CAN ID 的派生关系待协议定稿）
  rx_control_id: 0x101       # 接收控制帧 CAN ID（占位：待 CAN 协议定稿）
  tx_report_id: 0x181        # 参数回报帧 CAN ID（占位：待 CAN 协议定稿）

fault:
  oc_trip_a: "3 * motor.i_peak_10s_a"     # 过流阈值 [A] → 72.9
  vbus_ov_v: 36.0                         # 母线过压 [V]（24V 系统门限，1.5× 系统额定）
  vbus_uv_v: 9.0                          # 母线欠压 [V]（24V 系统门限）
  slow_debounce: 5                        # L2/L3 去抖次数（连续 1kHz tick）
  adc_stall_ms: 10                        # PMT 帧停滞超时 [ms]
  enc_err_delta: 3                        # 编码器错误增量阈值（每 1kHz tick 的错误计数增量）
  # —— 时序量：占位，待 FOC 时序体系定义后接入（当前代码使用内部宏：250 样本 / 50 tick）——
  # rms_window_samples: 250
  # settle_ticks: 50

control:
  # FOC 预留（数值待整定；当前无消费者）
  current_loop:
    kp: 0.0                # 电流环 PID（目标带宽 1kHz）
    ki: 0.0
  speed_loop:
    kp: 0.0                # 速度环 PID（目标带宽 50Hz）
    ki: 0.0
  limits:
    i_q_max_a: "motor.i_peak_10s_a"   # 电流限幅 [A]（RMS 口径，与 motor.i_peak_10s_a 一致；FOC 接入时确认）→ 24.3
    duty_max: 0.95                    # 占空比上限
```

---

## 5. 生成器设计（`scripts/gen_params.py`）

### 5.1 接口

```text
用法：gen_params.py --config-dir <dir> --out-dir <dir> [--check]

--check   仅校验（不写文件）；用于提交前/CI 快速检查
退出码：0 = 成功；非 0 = 校验或求值失败（消息格式 文件:行:键: 原因）
```

依赖：Python 3 + PyYAML（环境已具备 6.0.3）。

### 5.2 处理流程

```text
读取 3 个 YAML
  → 模式校验（必填/未知键/类型/范围/文档字段标记）
  → 收集符号表（file.path → 值/表达式）
  → 表达式解析（ast，白名单节点）+ 拓扑排序
  → 求值 + 类型强制 + 跨域一致性校验（§3.3）
  → 生成 params_generated.h / .c / params_report.txt
```

### 5.3 生成物形态

`params_generated.h`：

```c
/*
 * 自动生成 — 请勿手改！来源：config/*.yaml；生成器：scripts/gen_params.py
 * （不含时间戳，保证可复现构建）
 */
#ifndef PARAMS_GENERATED_H
#define PARAMS_GENERATED_H
#include "app_motor_params.h"
#include "app_hw_params.h"
#include "app_sw_params.h"
extern const app_motor_params_t g_motor_params_factory;
extern const app_hw_params_t    g_hw_params_factory;
extern const app_sw_params_t    g_sw_params_factory;
#endif
```

`params_generated.c`（设计初始化器 + 表达式留痕）：

```c
#include "params_generated.h"
const app_sw_params_t g_sw_params_factory = {
    .fault.oc_trip_a = 72.9f,   /* = 3 * motor.i_peak_10s_a */
    .fault.vbus_ov_v = 36.0f,
    .fault.vbus_uv_v = 9.0f,
    ...
};
```

- 浮点：`%.9g` + `f` 后缀；整数：`U` 后缀；
- 派生字段的表达式原文保留为行尾注释（审查/追溯）。

`params_report.txt`（全参数对照表，示例）：

```text
# 参数报告（构建生成，勿手改）
motor.pole_pairs                        = 10
hardware.current_sense.a_per_volt       = 66.6667     # 1 / (shunt_ohm * amp_gain)
software.can.rx_control_id              = 0x101
software.fault.oc_trip_a                = 72.9        # 3 * motor.i_peak_10s_a
...
```

### 5.4 字段模式表（生成器内置）

- 每域一张表：`(yaml_path, c_field, c_type, doc_only, min, max)`；
- `doc_only` 字段（`motor.name` / `motor.winding` / `encoder.model` / `board.name`）只做校验，不生成 C 字段；
- `HEX_FIELDS`（`software.can.rx_control_id` / `can.tx_report_id`）以十六进制输出/展示：生成物 `0x...U`，报告 `0x...`；
- 自测夹具位于 `scripts/tests/fixtures/`（与生产 `config/` 解耦；SCHEMA 变更时需同步）；
- 字段模式表与手写结构体的对应关系由**设计初始化器**保证：生成器引用了结构体不存在的字段 → **编译错误**（漂移即时暴露）。

---

## 6. C 侧 API 与结构体

### 6.1 结构体定义

```c
/* App/Control/app_motor_params.h */
typedef struct {
    uint8_t resolution_bits;
    uint8_t rotor_ring_teeth;
    uint8_t rotor_pinion_teeth;
    uint8_t output_pinion_teeth;
    float   rotor_ratio;
    float   output_ratio;
} app_motor_encoder_t;

typedef struct {
    uint8_t  pole_pairs;
    float    rs_ohm;
    float    ls_h;
    float    ke_vs_per_rad;
    float    kt_nm_per_a;
    float    i_rated_a;
    float    i_peak_10s_a;
    float    i_peak_2s_a;
    float    vbus_nom_v;
    uint16_t rpm_max;
    float    inertia_kgm2;
    float    torque_rated_nm;
    float    torque_peak_10s_nm;
    app_motor_encoder_t encoder;
} app_motor_params_t;

/* App/Platform/app_hw_params.h */
typedef struct { float shunt_ohm; float amp_gain; float a_per_volt; float bias_v; } app_hw_current_sense_t;
typedef struct { uint32_t divider_high_ohm; uint32_t divider_low_ohm; float v_per_volt; } app_hw_vbus_sense_t;
typedef struct { uint32_t pullup_ohm; float r25_ohm; float b_value_k; float max_ohm; } app_hw_ntc_t;
typedef struct { uint8_t levels; } app_hw_canid_t;
typedef struct { uint8_t sample_cycle; uint32_t trigger_delay_ns; } app_hw_adc_t;
typedef struct { uint32_t pwm_freq_hz; uint32_t deadtime_ns; } app_hw_inverter_t;

typedef struct {
    app_hw_current_sense_t current_sense;
    app_hw_vbus_sense_t    vbus_sense;
    app_hw_ntc_t           ntc;
    app_hw_canid_t         canid_dip;
    app_hw_adc_t           adc;
    app_hw_inverter_t      inverter;
} app_hw_params_t;

/* App/Platform/app_sw_params.h */
typedef struct {
    uint32_t baudrate;        /* CAN 波特率 [bps] */
    uint8_t  node_id_default; /* 默认节点号（占位：DIP 解码未实现；与 CAN ID 的派生关系待协议定稿） */
    uint32_t rx_control_id;   /* 接收控制帧 CAN ID（占位：待协议定稿） */
    uint32_t tx_report_id;    /* 参数回报帧 CAN ID（占位：待协议定稿） */
} app_sw_can_t;
typedef struct {
    float    oc_trip_a;
    float    vbus_ov_v;
    float    vbus_uv_v;
    uint16_t slow_debounce;
    uint16_t adc_stall_ms;
    uint8_t  enc_err_delta;
} app_sw_fault_t;
typedef struct { float kp; float ki; } app_sw_pid_t;
typedef struct { float i_q_max_a; float duty_max; } app_sw_limits_t;
typedef struct { app_sw_pid_t current_loop; app_sw_pid_t speed_loop; app_sw_limits_t limits; } app_sw_control_t;

typedef struct {
    app_sw_can_t     can;
    app_sw_fault_t   fault;
    app_sw_control_t control;
} app_sw_params_t;
```

### 6.2 访问器 API（三域统一形态）

```c
/** @brief 工厂默认参数（只读，指向生成常量） */
const app_motor_params_t *app_motor_params_default(void);
/** @brief 加载参数：工厂默认 +（将来）flash 覆盖；无失败路径 */
void app_motor_params_load(app_motor_params_t *out);
/* app_hw_params_default / app_hw_params_load、app_sw_params_default / app_sw_params_load 同构 */
```

### 6.3 消费接线

| 模块 | 加载 | 使用字段 | 替换的旧宏 |
| :--- | :--- | :--- | :--- |
| `app_analog_signal.c` | hw | `a_per_volt` / `v_per_volt` / `bias_v` / `pullup_ohm` / `max_ohm`（vref 用 `INTF_ADC_DEFAULT_VREF_MV`，驱动侧固定） | `APP_ANALOG_I_AMP_PER_VOLT` / `APP_ANALOG_VBUS_VOLT_PER_VOLT` / `APP_ANALOG_I_ZERO_VOLTS` / `APP_ANALOG_NTC_PULLUP_OHM` / `APP_ANALOG_NTC_MAX_OHM` / `APP_ANALOG_VREF_VOLTS` |
| `app_adc.c` | hw | `adc.trigger_delay_ns` / `adc.sample_cycle`（cfg 为 0/NULL 时） | `APP_ADC_TRIGGER_DELAY_NS_DEFAULT` / `APP_ADC_SAMPLE_CYCLE_DEFAULT` |
| `app_3phase_inverter.c` | hw | `inverter.pwm_freq_hz` / `inverter.deadtime_ns`（cfg 为 0/NULL 时） | `APP_3PHASE_INVERTER_FREQ_HZ_DEFAULT` / `APP_3PHASE_INVERTER_DEADTIME_NS_DEFAULT` |
| `app_fault.c` | sw + hw | sw：`fault.*` 阈值默认；hw：`current_sense.a_per_volt` + `INTF_ADC_DEFAULT_VREF_MV`（驱动侧固定，WDOG 原始窗口换算） | `APP_FAULT_OC_TRIP_A_DEFAULT` 等 6 项 / `APP_ANALOG_I_AMP_PER_VOLT`（引用） |
| `app_can.c` | sw | `can.baudrate` | `APP_CAN_BAUDRATE` |
| `app_debug_can.c` | sw | `can.tx_report_id`（1Hz 总线发送帧 ID；波特率打印） | `CAN_LB_ID` / `CAN_LB_BAUDRATE`（环回自检保留） |
| `app_debug_inverter.c` | hw | 打印用频率/死区（改读 hw 参数） | 打印中的 `APP_3PHASE_INVERTER_*_DEFAULT` |
| `app_logic.c` | 三域 | 启动参数摘要打印（见 §9.3）；控制节拍 = `inverter.pwm_freq_hz` | — |
| FOC（后续） | motor | 全部 | — |

**保留为内部宏（不进 YAML）**：

- `APP_FAULT_RMS_WINDOW_DEFAULT`（250）/ `APP_FAULT_SETTLE_TICKS_DEFAULT`（50）——待 FOC 时序体系；
- `APP_ANALOG_ZERO_CAL_*`（标定过程参数）；
- `APP_ADC_TRIGGER_CMP_INDEX`、引脚/通道映射（Board 层）。

> 节拍与开关频率统一由 `inverter.pwm_freq_hz` 驱动：主循环节拍（`app_logic`）与模拟量采样率（`app_analog_signal`）均由该值派生，不再保留 `APP_ANALOG_SAMPLE_RATE_HZ` / `APP_LOOP_FREQ_HZ` 等独立节拍宏。

### 6.4 旧宏退场清单

§6.3 表格中"替换的旧宏"列出的宏**全部删除**（含 `app_analog_signal.h` 中对 `app_fault` 暴露的 `APP_ANALOG_I_AMP_PER_VOLT`——`app_fault` 改从 hw 参数取标度）。

---

## 7. flash 覆盖预留（v2，不实现）

```c
/* App/Platform/app_sw_params.c */
void app_sw_params_load(app_sw_params_t *out) {
    *out = g_sw_params_factory;
    /* TODO(v2): app_param_is_ready() → app_param_load(APP_PARAM_KEY_SW, &overlay)
     *           → 字段级叠加（整定/自校准结果） */
}
```

- 三域各预留一个 flash key（`APP_PARAM_KEY_MOTOR` / `_HW` / `_SW`，v2 登记）；
- 覆盖语义：工厂默认 → flash 覆盖（字段级，版本化结构体）；
- 在线辨识（Rs/Ls/Ke）→ motor 域；PID 整定 → sw.control；保护阈值微调 → sw.fault；
- 现有标定值（编码器零点）保持独立 key，不并入本管线。

---

## 8. 构建集成（CMake）

```cmake
# 参数管线：configure 期生成（失败即中止配置）
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PARAMS_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen_params.py
            --config-dir ${CMAKE_SOURCE_DIR}/config --out-dir ${PARAMS_GEN_DIR}
    RESULT_VARIABLE _params_rc)
if(NOT _params_rc EQUAL 0)
    message(FATAL_ERROR "参数生成失败（scripts/gen_params.py）")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/config/motor.yaml
    ${CMAKE_SOURCE_DIR}/config/hardware.yaml
    ${CMAKE_SOURCE_DIR}/config/software.yaml
    ${CMAKE_SOURCE_DIR}/scripts/gen_params.py)

sdk_app_inc(${PARAMS_GEN_DIR})                      # 已验证支持绝对路径
sdk_app_src(${PARAMS_GEN_DIR}/params_generated.c)   # 已验证支持绝对路径
```

- YAML/脚本改动 → 构建系统自动重跑 cmake（`CMAKE_CONFIGURE_DEPENDS`）→ 重新生成并重编；
- `build/` 已被 `.gitignore` 排除，生成物不提交；
- 生成物不含时间戳（可复现构建）。

---

## 9. 验证计划

### 9.1 生成器（构建前，手工）

| 用例 | 期望 |
| :--- | :--- |
| 真实 `config/`（生产配置） | 仅 `--check` 通过（值不做断言；夹具见 §5.4） |
| 正常三文件（夹具） | 生成成功；report 值 = 手算（66.6667 / 22.2121 / 72.9 / 36.0 / 0.98） |
| 删除必填键 | 报错：文件:键:缺失 |
| 增加未知键 | 报错：文件:行:键:未知键（键名定位） |
| 表达式引用未知符号 | 报错：未定义符号 |
| 构造环引用（A→B→A） | 报错：环引用路径 |
| 类型不符（整数键给 1.5） | 报错：类型 |
| `oc_trip_a` 改为 200 | 报错：超满量程（规则 4） |

### 9.2 构建

- `make build` 零告警；`build/generated/` 三文件存在；report 与 YAML 一致。

### 9.3 台架

- 启动参数摘要打印与 YAML 一致（新增一行，见下）；
- `f` 故障阈值默认值不变（72.9 / 36 / 9 / 5 / 10 / 3）；
- `d` 模拟量读数与迁移前一致（母线 ≈23.7V、电流零点、NTC）；
- 逆变器输出 25kHz / 50ns（自检打印）；
- 手动欠压复测：检出 → 锁存 → 恢复 → 清除，链路无回归；
- ADC 序列（1kHz）与 PMT（25kHz）节拍无回归。

启动摘要（app_init 中，验证管线端到端）：

```text
params: pp=10 rs=0.1580 ls=0.0001185 | a/v=66.6667 vbus/v=22.2121 | oc=72.9 ov=36.0 uv=9.0 | pwm=25000/50
```

### 9.4 回滚

单提交回滚（`git revert`）；旧宏在回滚后完整恢复。

---

## 10. 预留与未接入项

| 项 | 状态 | 说明 |
| :--- | :--- | :--- |
| NTC 型号（`r25_ohm` / `b_value_k`） | 占位值 | 选型后修正；换算逻辑另行实现 |
| CANID 解码（`canid_dip` / `can.node_id_default`） | 占位 | 硬件确认电阻网络解码关系后填充 |
| CAN 控制/回报 ID（`can.rx_control_id` / `tx_report_id`） | 占位 | `tx_report_id` 已接线 CAN 自检周期帧；`rx_control_id` 待协议逻辑；如需按节点派生可改为表达式（如 `"0x100 + node_id_default"`）；当前 SCHEMA 限标准帧（≤0x7FF）；扩展帧待协议定义帧格式字段后放宽 |
| RMS 窗口 / 静默期 | YAML 注释占位 | 待 FOC 时序体系定义后接入（当前内部宏） |
| flash 覆盖 | API 预留 | v2；key 未登记；load() 落地时需防御 0 值（如 pwm_freq_hz） |
| FOC PID / 限幅 | 生成未消费 | 整定后填值 |
| 调试开关（`periodic_print` 等） | 未纳入 | 编译期开关，保持现状 |
| 零点标定参数（`ZERO_CAL_*`） | 未纳入 | 标定过程参数，保持内部 |
| ADC 基准（`INTF_ADC_DEFAULT_VREF_MV`，驱动侧固定 3.3V，不进 YAML） | 唯一来源 | app 侧换算（analog/fault）与 ADC 配置均以其为准 |
| 引脚/通道映射 | 不入 YAML | Board 层 |

---

## 11. 风险与缓解

| 风险 | 缓解 |
| :--- | :--- |
| 生成器字段表与手写结构体漂移 | 设计初始化器：引用不存在字段 → 编译错；缺字段 → report 对照 + review |
| 双源残留（YAML 与旧宏并存） | §6.4 退场清单逐项闭环；实施后 grep 旧宏为 0 |
| 构建依赖 Python3 + PyYAML | 环境已具备（6.0.3）；README 注明；`--check` 可独立运行 |
| configure 期生成失败阻塞构建 | 预期行为（参数错误必须显式暴露）；错误消息含文件:行:键:原因 |
| 表达式求值安全 | 受限命名空间（无 `__builtins__`）、节点白名单；输入为仓库内文件 |
| YAML 编辑 → 全量重编 | `CMAKE_CONFIGURE_DEPENDS` 触发 cmake 重跑（秒级）；可接受 |

---

## 12. 实施顺序（供计划拆分）

1. 生成器 + 三个 YAML + 生成物（可独立验证：`--check` + report 对照）；
2. CMake 接入 + 空结构体编译（验证生成源进构建）；
3. 三个 params 模块（类型 + 访问器 + 工厂常量对接）；
4. 消费接线迁移（analog → adc → inverter → fault → can）+ 旧宏删除；
5. 启动摘要打印 + 台架验证 + 文档收尾（bringup §6.4 加指向本设计的注记）。
