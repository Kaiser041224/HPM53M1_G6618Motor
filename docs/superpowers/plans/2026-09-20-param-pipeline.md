# 参数管线实施计划（YAML 三域 → 工厂默认参数）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把散落的参数常量迁移到 `config/*.yaml`（三域），构建期由生成器求值 + 校验 → 类型化 C 参数结构体（方案 B），为将来 flash 覆盖（在线辨识/整定）预留 `*_load()` API。

**Architecture:** `config/{motor,hardware,software}.yaml` →（configure 期，Python/PyYAML）→ `build/generated/params_generated.{h,c}` + 报告 → 三个手写 params 模块（`app_motor_params` / `app_hardware_params` / `app_software_params`，提供 `default()/load()`）→ 现有模块在 init 时加载（保持"NULL = 默认"语义）。

**Tech Stack:** C17、CMake 3.13+、Python 3 + PyYAML 6.0.3（环境已具备）、HPM SDK。

**Spec:** `docs/superpowers/specs/2026-09-20-param-pipeline-design.md`

---

## 文件结构

| 动作 | 文件 | 职责 |
| :--- | :--- | :--- |
| Create | `config/motor.yaml` | 机械参数（电机 + 编码器关系） |
| Create | `config/hardware.yaml` | 硬件参数（采样电路 + ADC 时序 + 功率级） |
| Create | `config/software.yaml` | 软件参数（保护阈值 + CAN + FOC 预留） |
| Create | `scripts/gen_params.py` | 生成器：模式/表达式/跨域校验 + 生成 C/报告 |
| Create | `App/Control/Inc/app_motor_params.h` / `Src/app_motor_params.c` | 机械参数类型 + 访问器 |
| Create | `App/Platform/Inc/app_hardware_params.h` / `Src/app_hardware_params.c` | 硬件参数类型 + 访问器 |
| Create | `App/Platform/Inc/app_software_params.h` / `Src/app_software_params.c` | 软件参数类型 + 访问器 |
| Modify | `CMakeLists.txt` | configure 期生成 + 生成源/头进构建 |
| Modify | `App/Control/Src/app_fault.c` | 阈值默认 → sw/hw 参数 |
| Modify | `App/Platform/Src/app_analog_signal.c` + `Inc/app_analog_signal.h` | 换算常数 → hw 参数；删 `APP_ANALOG_I_AMP_PER_VOLT` |
| Modify | `App/Platform/Src/app_adc.c` + `Inc/app_adc.h` | 默认 → hw 参数；删 2 宏 |
| Modify | `App/Platform/Src/app_3phase_inverter.c` + `Inc/app_3phase_inverter.h` | 默认 → hw 参数；删 2 宏 |
| Modify | `App/Debug/Src/app_debug_inverter.c`、`app_debug_motor.c` | 引用改为 hw 参数 |
| Modify | `App/Platform/Src/app_can.c` | 波特率 → sw 参数；删宏 |
| Modify | `App/Logic/app_logic.c` | 启动参数摘要打印 |
| Modify | `docs/superpowers/specs/2026-09-18-m1-board-bringup-design.md` | §6.4 加"已被取代"注记 |

---

## Task 1: 三个 YAML 配置文件

**Files:**
- Create: `config/motor.yaml`
- Create: `config/hardware.yaml`
- Create: `config/software.yaml`

- [ ] **Step 1: 写入 `config/motor.yaml`**

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

- [ ] **Step 2: 写入 `config/hardware.yaml`**

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
  sample_cycle: 25                        # 采样窗口 [ADC 时钟数]（SDK 最小 10；曾复现通道数据重复，勿低于 10）
  trigger_delay_ns: 500                   # 谷底后触发延时 [ns]

inverter:
  pwm_freq_hz: 25000                      # 开关频率 [Hz]
  deadtime_ns: 50                         # HPM 侧死区 [ns]（预驱自带 50~250ns）
```

- [ ] **Step 3: 写入 `config/software.yaml`**

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

- [ ] **Step 4: YAML 语法自检**

Run:
```bash
python3 -c "import yaml; [yaml.safe_load(open(f'config/{n}.yaml', encoding='utf-8')) for n in ('motor','hardware','software')]; print('YAML OK')"
```
Expected: `YAML OK`

- [ ] **Step 5: 提交**

```bash
git add config/motor.yaml config/hardware.yaml config/software.yaml
git commit -m "feat(params): 新增三域 YAML 出厂参数（机械/硬件/软件，含表达式与占位项）"
```

---

## Task 2: 生成器 `scripts/gen_params.py`

**Files:**
- Create: `scripts/gen_params.py`

- [ ] **Step 1: 写入完整生成器**

```python
#!/usr/bin/env python3
"""参数管线生成器：config/*.yaml（三域）→ params_generated.{h,c} + params_report.txt

用法：
    python3 scripts/gen_params.py --config-dir config --out-dir build/generated [--check]

- 表达式：引号字符串按计算式求值（构建期）；引用支持 同文件叶子名 / 节.字段 / 跨文件 域.路径
- 校验：模式（必填/未知键/类型/范围）+ 跨域一致性；任一失败 → 非零退出 + 文件:行:键:原因
- 生成物不含时间戳（可复现构建）
"""
from __future__ import annotations

import argparse
import ast
import math
import sys
from pathlib import Path

try:
    import yaml
except ImportError as e:  # 环境错误：缺 PyYAML
    print(f"[params] 错误：缺少 PyYAML（{e}）；请安装 python3-yaml 或 pip install PyYAML", file=sys.stderr)
    sys.exit(1)

DOMAINS = ("motor", "hardware", "software")
FILES = {d: f"{d}.yaml" for d in DOMAINS}

# 字段模式表：(yaml 路径, C 字段路径, C 类型, 最小值, 最大值)；None = 不限
# C 类型：u8/u16/u32/f32
SCHEMA = {
    "motor": [
        ("pole_pairs", "pole_pairs", "u8", 1, 255),
        ("rs_ohm", "rs_ohm", "f32", 0.0, None),
        ("ls_h", "ls_h", "f32", 0.0, None),
        ("ke_vs_per_rad", "ke_vs_per_rad", "f32", 0.0, None),
        ("kt_nm_per_a", "kt_nm_per_a", "f32", 0.0, None),
        ("i_rated_a", "i_rated_a", "f32", 0.0, None),
        ("i_peak_10s_a", "i_peak_10s_a", "f32", 0.0, None),
        ("i_peak_2s_a", "i_peak_2s_a", "f32", 0.0, None),
        ("vbus_nom_v", "vbus_nom_v", "f32", 0.0, None),
        ("rpm_max", "rpm_max", "u16", 1, 65535),
        ("inertia_kgm2", "inertia_kgm2", "f32", 0.0, None),
        ("torque_rated_nm", "torque_rated_nm", "f32", 0.0, None),
        ("torque_peak_10s_nm", "torque_peak_10s_nm", "f32", 0.0, None),
        ("encoder.resolution_bits", "encoder.resolution_bits", "u8", 1, 32),
        ("encoder.rotor_ring_teeth", "encoder.rotor_ring_teeth", "u8", 1, 255),
        ("encoder.rotor_pinion_teeth", "encoder.rotor_pinion_teeth", "u8", 1, 255),
        ("encoder.output_pinion_teeth", "encoder.output_pinion_teeth", "u8", 1, 255),
        ("encoder.rotor_ratio", "encoder.rotor_ratio", "f32", 0.0, None),
        ("encoder.output_ratio", "encoder.output_ratio", "f32", 0.0, None),
    ],
    "hardware": [
        ("current_sense.shunt_ohm", "current_sense.shunt_ohm", "f32", 0.0, None),
        ("current_sense.amp_gain", "current_sense.amp_gain", "f32", 0.0, None),
        ("current_sense.a_per_volt", "current_sense.a_per_volt", "f32", 0.0, None),
        ("current_sense.bias_v", "current_sense.bias_v", "f32", 0.0, None),
        ("vbus_sense.divider_high_ohm", "vbus_sense.divider_high_ohm", "u32", 1, None),
        ("vbus_sense.divider_low_ohm", "vbus_sense.divider_low_ohm", "u32", 1, None),
        ("vbus_sense.v_per_volt", "vbus_sense.v_per_volt", "f32", 1.0, None),
        ("ntc.pullup_ohm", "ntc.pullup_ohm", "u32", 1, None),
        ("ntc.r25_ohm", "ntc.r25_ohm", "f32", 0.0, None),
        ("ntc.b_value_k", "ntc.b_value_k", "f32", 0.0, None),
        ("ntc.max_ohm", "ntc.max_ohm", "f32", 0.0, None),
        ("canid_dip.levels", "canid_dip.levels", "u8", 1, 255),
        ("adc.sample_cycle", "adc.sample_cycle", "u8", 1, 255),
        ("adc.trigger_delay_ns", "adc.trigger_delay_ns", "u32", 0, None),
        ("inverter.pwm_freq_hz", "inverter.pwm_freq_hz", "u32", 1, None),
        ("inverter.deadtime_ns", "inverter.deadtime_ns", "u32", 0, None),
    ],
    "software": [
        ("can.baudrate", "can.baudrate", "u32", 1, None),
        ("can.node_id_default", "can.node_id_default", "u8", 0, 127),
        # CAN ID 暂限标准帧（11-bit ≤0x7FF）；扩展帧待协议定义帧格式字段后放宽
        ("can.rx_control_id", "can.rx_control_id", "u32", 0, 0x7FF),
        ("can.tx_report_id", "can.tx_report_id", "u32", 0, 0x7FF),
        ("fault.oc_trip_a", "fault.oc_trip_a", "f32", 0.0, None),
        ("fault.vbus_ov_v", "fault.vbus_ov_v", "f32", 0.0, None),
        ("fault.vbus_uv_v", "fault.vbus_uv_v", "f32", 0.0, None),
        ("fault.slow_debounce", "fault.slow_debounce", "u16", 1, 65535),
        ("fault.adc_stall_ms", "fault.adc_stall_ms", "u16", 1, 65535),
        ("fault.enc_err_delta", "fault.enc_err_delta", "u8", 1, 255),
        ("control.current_loop.kp", "control.current_loop.kp", "f32", None, None),
        ("control.current_loop.ki", "control.current_loop.ki", "f32", None, None),
        ("control.speed_loop.kp", "control.speed_loop.kp", "f32", None, None),
        ("control.speed_loop.ki", "control.speed_loop.ki", "f32", None, None),
        ("control.limits.i_q_max_a", "control.limits.i_q_max_a", "f32", 0.0, None),
        ("control.limits.duty_max", "control.limits.duty_max", "f32", 0.0, 1.0),
    ],
}

# 仅文档字段：(yaml 路径, 允许值集合或 None)；只校验，不生成 C 字段
DOC_ONLY = {
    "motor": [("name", None), ("winding", {"star", "delta"}), ("encoder.model", None)],
    "hardware": [("board.name", None)],
    "software": [],
}

C_TYPE_RANGE = {"u8": (0, 255), "u16": (0, 65535), "u32": (0, 4294967295)}
STRUCT_NAMES = {"motor": "app_motor_params_t", "hardware": "app_hardware_params_t", "software": "app_software_params_t"}
CONST_NAMES = {"motor": "g_motor_params_factory", "hardware": "g_hardware_params_factory", "software": "g_software_params_factory"}

ALLOWED_FUNCS = {"sqrt": math.sqrt, "sin": math.sin, "cos": math.cos,
                 "atan2": math.atan2, "min": min, "max": max, "abs": abs}
ALLOWED_CONSTS = {"pi": math.pi}

# 以十六进制输出/展示的字段（生成物与报告）；键为 "域.路径"
HEX_FIELDS = {"software.can.rx_control_id", "software.can.tx_report_id"}


def _is_hex(domain: str, path: str) -> bool:
    return f"{domain}.{path}" in HEX_FIELDS


class ParamError(Exception):
    pass


def err(where: str, msg: str) -> ParamError:
    return ParamError(f"{where}: {msg}")


def where_of(domain: str, path: str, marks) -> str:
    mark = marks.get(path)
    if mark is not None:
        return f"{FILES[domain]}:{mark[0]}:{path}"
    return f"{FILES[domain]}:{path}"


class _MarkedDict(dict):
    """构造时记录每个键的起始位置（行/列），供错误信息使用。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.key_marks = {}


class _DuplicateKey(Exception):
    """YAML 映射重复键；在 load_domain 中统一为 文件:行:键: 重复键 错误。"""

    def __init__(self, key, line):
        super().__init__(key, line)
        self.key = key
        self.line = line


class _StrictLoader(yaml.SafeLoader):
    """SafeLoader + 重复键拒绝（避免静默覆盖）。"""


def _construct_mapping_strict(loader, node, deep=False):
    mapping = _MarkedDict()
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise _DuplicateKey(key, key_node.start_mark.line + 1)
        mapping[key] = loader.construct_object(value_node, deep=deep)
        mapping.key_marks[key] = (key_node.start_mark.line + 1, key_node.start_mark.column + 1)
    return mapping


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping_strict)


def flatten(node, filename, prefix=(), out=None, marks=None):
    if out is None:
        out = {}
    if marks is None:
        marks = {}
    if not isinstance(node, dict):
        raise err(filename, f"根节点必须是映射（实际 {type(node).__name__}）")
    for k, v in node.items():
        if not isinstance(k, str):
            raise err(filename, f"键必须是字符串：{k!r}")
        path = prefix + (k,)
        if isinstance(v, dict):
            flatten(v, filename, path, out, marks)
        else:
            key = ".".join(path)
            out[key] = v
            if isinstance(node, _MarkedDict):
                mark = node.key_marks.get(k)
                if mark is not None:
                    marks[key] = mark
    return out, marks


def load_domain(config_dir: Path, domain: str) -> tuple:
    p = config_dir / FILES[domain]
    if not p.is_file():
        raise err(str(p), "文件不存在")
    try:
        with p.open("r", encoding="utf-8") as f:
            data = yaml.load(f, Loader=_StrictLoader)
    except yaml.YAMLError as e:
        raise err(str(p), f"YAML 解析失败：{e}")
    except _DuplicateKey as e:
        raise err(f"{p}:{e.line}:{e.key}", "重复键")
    except ParamError as e:
        raise err(str(p), str(e))
    if data is None:
        raise err(str(p), "文件为空")
    return flatten(data, str(p))


def check_literal(where: str, val, ctype: str, lo, hi) -> None:
    if ctype == "f32":
        if isinstance(val, bool) or not isinstance(val, (int, float)):
            raise err(where, f"必须是数值（实际 {val!r}）")
        try:
            v = float(val)
        except OverflowError:
            raise err(where, "数值超出 float 表示范围")
        if not math.isfinite(v):
            raise err(where, "必须是有限值")
        if lo is not None and v < lo:
            raise err(where, f"小于下限 {lo}（实际 {v}）")
        if hi is not None and v > hi:
            raise err(where, f"大于上限 {hi}（实际 {v}）")
        return
    if isinstance(val, bool) or not isinstance(val, int):
        raise err(where, f"必须是整数（实际 {val!r}）")
    tlo, thi = C_TYPE_RANGE[ctype]
    lo2 = tlo if lo is None else max(lo, tlo)
    hi2 = thi if hi is None else min(hi, thi)
    if not (lo2 <= val <= hi2):
        raise err(where, f"越界：{val}（允许 {lo2}..{hi2}）")


def validate_domain(domain: str, flat: dict, marks) -> None:
    spec = {path: (ctype, lo, hi) for (path, _cfield, ctype, lo, hi) in SCHEMA[domain]}
    docs = dict(DOC_ONLY[domain])
    for path in flat:
        if path not in spec and path not in docs:
            raise err(where_of(domain, path, marks), "未知键（检查拼写；模式表中无此字段）")
    for path in list(spec) + list(docs):
        if path not in flat:
            raise err(f"{FILES[domain]}:{path}", "缺少必填键")
    for path, val in flat.items():
        where = where_of(domain, path, marks)
        if path in docs:
            allowed = docs[path]
            if allowed is not None:
                if not isinstance(val, str) or val not in allowed:
                    raise err(where, f"取值必须是 {sorted(allowed)} 之一（实际 {val!r}）")
            elif not isinstance(val, str):
                raise err(where, f"必须是字符串（实际 {type(val).__name__}）")
            continue
        ctype, lo, hi = spec[path]
        if isinstance(val, str):
            continue  # 表达式：求值后校验
        check_literal(where, val, ctype, lo, hi)


class Symbol:
    def __init__(self, domain: str, path: str, raw, ctype: str, lo, hi, mark=None):
        self.domain = domain
        self.path = path
        self.raw = raw
        self.ctype = ctype
        self.lo = lo
        self.hi = hi
        self.mark = mark
        self.value = None
        self.expr = raw if isinstance(raw, str) else None

    @property
    def where(self) -> str:
        marks = {} if self.mark is None else {self.path: self.mark}
        return where_of(self.domain, self.path, marks)


def build_symbols(flats: dict, all_marks):
    symbols = {d: {} for d in DOMAINS}
    leaves = {d: {} for d in DOMAINS}
    for d in DOMAINS:
        spec = {path: (ctype, lo, hi) for (path, _cfield, ctype, lo, hi) in SCHEMA[d]}
        marks = all_marks[d]
        for path, raw in flats[d].items():
            if path not in spec:
                continue  # doc_only
            ctype, lo, hi = spec[path]
            symbols[d][path] = Symbol(d, path, raw, ctype, lo, hi, marks.get(path))
            leaf = path.split(".")[-1]
            leaves[d].setdefault(leaf, []).append(path)
    return symbols, leaves


def coerce(val, where: str, ctype: str, lo, hi):
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        raise err(where, f"表达式结果必须是数值（实际 {val!r}）")
    try:
        fv = float(val)
    except OverflowError:
        raise err(where, "数值超出 float 表示范围")
    if not math.isfinite(fv):
        raise err(where, "必须是有限值")
    if ctype == "f32":
        v = fv
        if lo is not None and v < lo:
            raise err(where, f"小于下限 {lo}（实际 {v}）")
        if hi is not None and v > hi:
            raise err(where, f"大于上限 {hi}（实际 {v}）")
        return v
    r = round(fv)
    if abs(fv - r) > 1e-9:
        raise err(where, f"整数键的表达式结果必须是整值（实际 {val}）")
    tlo, thi = C_TYPE_RANGE[ctype]
    lo2 = tlo if lo is None else max(lo, tlo)
    hi2 = thi if hi is None else min(hi, thi)
    if not (lo2 <= r <= hi2):
        raise err(where, f"越界：{r}（允许 {lo2}..{hi2}）")
    return int(r)


def dotted_name(node, where: str) -> str:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return dotted_name(node.value, where) + "." + node.attr
    raise err(where, "不支持的引用形式")


BINOPS = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a / b,
    ast.Pow: lambda a, b: a ** b,
}
UNARYOPS = {ast.UAdd: lambda a: a, ast.USub: lambda a: -a}


def resolve_ref(name: str, domain: str, symbols, leaves, stack, where: str):
    parts = name.split(".")
    if parts[0] in DOMAINS:  # 跨域：域.路径
        d = parts[0]
        path = ".".join(parts[1:])
        if path in symbols[d]:
            return eval_symbol(symbols[d][path], symbols, leaves, stack)
        raise err(where, f"未定义符号 {name!r}（{FILES[d]} 中无 {path}）")
    if name in symbols[domain]:
        return eval_symbol(symbols[domain][name], symbols, leaves, stack)
    if "." not in name and name in leaves[domain]:
        matches = leaves[domain][name]
        if len(matches) > 1:
            raise err(where, f"符号 {name!r} 在 {FILES[domain]} 中有歧义（候选：{', '.join(matches)}）")
        return eval_symbol(symbols[domain][matches[0]], symbols, leaves, stack)
    raise err(where, f"未定义符号 {name!r}")


def eval_node(node, domain: str, symbols, leaves, stack, where: str):
    if isinstance(node, ast.Constant):
        if isinstance(node.value, bool) or not isinstance(node.value, (int, float)):
            raise err(where, f"仅支持数值常量（实际 {node.value!r}）")
        return node.value
    if isinstance(node, ast.Name):
        if node.id in ALLOWED_CONSTS:
            return ALLOWED_CONSTS[node.id]
        return resolve_ref(node.id, domain, symbols, leaves, stack, where)
    if isinstance(node, ast.Attribute):
        return resolve_ref(dotted_name(node, where), domain, symbols, leaves, stack, where)
    if isinstance(node, ast.BinOp):
        op = BINOPS.get(type(node.op))
        if op is None:
            raise err(where, "不支持的运算符（允许 + - * / **）")
        lhs = eval_node(node.left, domain, symbols, leaves, stack, where)
        rhs = eval_node(node.right, domain, symbols, leaves, stack, where)
        if isinstance(node.op, ast.Pow) and isinstance(rhs, int) and abs(rhs) > 1000:
            raise err(where, f"幂指数过大：{rhs}（上限 1000）")
        return op(lhs, rhs)
    if isinstance(node, ast.UnaryOp):
        op = UNARYOPS.get(type(node.op))
        if op is None:
            raise err(where, "不支持的一元运算符")
        return op(eval_node(node.operand, domain, symbols, leaves, stack, where))
    if isinstance(node, ast.Call):
        if not isinstance(node.func, ast.Name) or node.func.id not in ALLOWED_FUNCS:
            raise err(where, "仅允许白名单函数：sqrt/sin/cos/atan2/min/max/abs")
        args = [eval_node(a, domain, symbols, leaves, stack, where) for a in node.args]
        return ALLOWED_FUNCS[node.func.id](*args)
    raise err(where, f"不支持的表达式语法：{type(node).__name__}")


def eval_symbol(sym: Symbol, symbols, leaves, stack: list):
    if sym.value is not None:
        return sym.value
    if sym.expr is None:
        sym.value = float(sym.raw) if sym.ctype == "f32" else int(sym.raw)
        return sym.value
    key = f"{sym.domain}.{sym.path}"
    if key in stack:
        chain = " -> ".join(stack[stack.index(key):] + [key])
        raise err(sym.where, f"环引用：{chain}")
    stack.append(key)
    try:
        tree = ast.parse(sym.expr, mode="eval")
        val = eval_node(tree.body, sym.domain, symbols, leaves, stack, sym.where)
    except ParamError:
        raise
    except SyntaxError as e:
        raise err(sym.where, f"表达式语法错误：{e.msg}")
    except ZeroDivisionError:
        raise err(sym.where, "表达式中出现除零")
    except (TypeError, ValueError, OverflowError) as e:
        raise err(sym.where, f"表达式求值失败：{e}")
    finally:
        stack.pop()
    sym.value = coerce(val, sym.where, sym.ctype, sym.lo, sym.hi)
    return sym.value


def cross_checks(symbols) -> None:
    def g(domain, path):
        return symbols[domain][path].value

    if not (g("software", "fault.vbus_uv_v") < g("software", "fault.vbus_ov_v")):
        raise err("跨域校验", "要求 vbus_uv_v < vbus_ov_v")
    if g("software", "fault.vbus_ov_v") > g("motor", "vbus_nom_v"):
        raise err("跨域校验", "vbus_ov_v 超过电机额定电压（motor.vbus_nom_v）")
    if g("software", "fault.oc_trip_a") < g("motor", "i_rated_a"):
        raise err("跨域校验", "oc_trip_a 低于额定电流（会误报）")
    full_scale = g("hardware", "current_sense.bias_v") * g("hardware", "current_sense.a_per_volt")
    if g("software", "fault.oc_trip_a") > full_scale:
        raise err("跨域校验", f"oc_trip_a 超过采样链路满量程（{full_scale:.1f} A）")
    if g("hardware", "adc.sample_cycle") < 10:
        raise err("跨域校验", "adc.sample_cycle 低于 SDK 最小值 10")
    period_ns = 1e9 / g("hardware", "inverter.pwm_freq_hz")
    if g("hardware", "inverter.deadtime_ns") >= period_ns / 2:
        raise err("跨域校验", f"inverter.deadtime_ns 超过半开关周期（{period_ns / 2:.0f} ns）")
    if g("hardware", "inverter.deadtime_ns") > 0.05 * period_ns:
        print(f"[params] 警告：inverter.deadtime_ns={g('hardware', 'inverter.deadtime_ns'):.0f} ns "
              f"超过开关周期 5%", file=sys.stderr)


def fmt_value(sym: Symbol) -> str:
    if sym.ctype == "f32":
        s = f"{sym.value:.9g}"
        if "." not in s and "e" not in s and "E" not in s:
            s += ".0"
        return s + "f"
    if _is_hex(sym.domain, sym.path):
        return f"0x{int(sym.value):X}U"
    return f"{int(sym.value)}U"


def gen_header() -> str:
    return """/*
 * 自动生成 — 请勿手改！
 * 来源：config/motor.yaml / config/hardware.yaml / config/software.yaml
 * 生成器：scripts/gen_params.py（构建期执行；不含时间戳，可复现）
 */
#ifndef PARAMS_GENERATED_H
#define PARAMS_GENERATED_H

#include "app_motor_params.h"
#include "app_hardware_params.h"
#include "app_software_params.h"

extern const app_motor_params_t g_motor_params_factory;
extern const app_hardware_params_t    g_hardware_params_factory;
extern const app_software_params_t    g_software_params_factory;

#endif /* PARAMS_GENERATED_H */
"""


def gen_source(symbols) -> str:
    lines = ['#include "params_generated.h"', ""]
    for d in DOMAINS:
        lines.append(f"const {STRUCT_NAMES[d]} {CONST_NAMES[d]} = {{")
        for path, cfield, _ctype, _lo, _hi in SCHEMA[d]:
            sym = symbols[d][path]
            comment = f"  /* = {sym.expr} */" if sym.expr else ""
            lines.append(f"    .{cfield} = {fmt_value(sym)},{comment}")
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def gen_report(symbols, flats) -> str:
    lines = ["# 参数报告（构建生成，勿手改）"]
    for d in DOMAINS:
        for path, _cfield, ctype, _lo, _hi in SCHEMA[d]:
            sym = symbols[d][path]
            if ctype == "f32":
                v = f"{sym.value:.9g}"
            elif _is_hex(d, path):
                v = f"0x{int(sym.value):X}"
            else:
                v = str(sym.value)
            expr = f"    # {sym.expr}" if sym.expr else ""
            lines.append(f"{d}.{path} = {v}{expr}")
        for path, _allowed in DOC_ONLY[d]:
            lines.append(f"{d}.{path} = {flats[d][path]}    # (仅文档)")
    return "\n".join(lines) + "\n"


def write_if_changed(path: Path, text: str) -> None:
    """内容不变则不落盘（保持 mtime，避免无谓的重编译）。"""
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="参数管线生成器（YAML → C）")
    ap.add_argument("--config-dir", required=True, type=Path)
    ap.add_argument("--out-dir", type=Path, default=None, help="输出目录（非 --check 模式必需）")
    ap.add_argument("--check", action="store_true", help="仅校验，不写文件")
    args = ap.parse_args(argv)

    try:
        loaded = {d: load_domain(args.config_dir, d) for d in DOMAINS}
        flats = {d: loaded[d][0] for d in DOMAINS}
        all_marks = {d: loaded[d][1] for d in DOMAINS}
        for d in DOMAINS:
            validate_domain(d, flats[d], all_marks[d])
        symbols, leaves = build_symbols(flats, all_marks)
        for d in DOMAINS:
            for path in symbols[d]:
                eval_symbol(symbols[d][path], symbols, leaves, [])
        cross_checks(symbols)
    except ParamError as e:
        print(f"[params] 错误：{e}", file=sys.stderr)
        return 1

    if args.check:
        print("[params] 校验通过（--check，未写文件）")
        return 0

    if args.out_dir is None:
        print("[params] 错误：缺少 --out-dir（非 --check 模式必需）", file=sys.stderr)
        return 1

    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_if_changed(args.out_dir / "params_generated.h", gen_header())
    write_if_changed(args.out_dir / "params_generated.c", gen_source(symbols))
    write_if_changed(args.out_dir / "params_report.txt", gen_report(symbols, flats))
    n = sum(len(symbols[d]) for d in DOMAINS)
    print(f"[params] 生成 {n} 个参数"
          f"（motor {len(symbols['motor'])} / hardware {len(symbols['hardware'])} / software {len(symbols['software'])}）"
          f" → {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: 语法检查**

Run: `python3 -m py_compile scripts/gen_params.py && echo "PY OK"`
Expected: `PY OK`

- [ ] **Step 3: `--check` 校验通过**

Run: `python3 scripts/gen_params.py --config-dir config --out-dir /tmp/opencode/params_gen --check`
Expected: `[params] 校验通过（--check，未写文件）`（退出码 0）

- [ ] **Step 4: 正常生成 + 报告核对**

Run:
```bash
python3 scripts/gen_params.py --config-dir config --out-dir /tmp/opencode/params_gen && cat /tmp/opencode/params_gen/params_report.txt
```
Expected（关键行）：
```text
hardware.current_sense.a_per_volt = 66.6666667    # 1 / (shunt_ohm * amp_gain)
hardware.current_sense.bias_v = 1.65
hardware.vbus_sense.v_per_volt = 22.2121212    # (divider_high_ohm + divider_low_ohm) / divider_low_ohm
motor.encoder.output_ratio = 0.98    # rotor_ring_teeth / output_pinion_teeth
software.fault.oc_trip_a = 72.9    # 3 * motor.i_peak_10s_a
software.fault.vbus_ov_v = 36
```

- [ ] **Step 5: 负例测试（6 例，隔离副本）**

Run:
```bash
rm -rf /tmp/opencode/params_neg && mkdir -p /tmp/opencode/params_neg && cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/
# 例1 未知键
printf 'bogus_key: 1\n' >> /tmp/opencode/params_neg/motor.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：motor.yaml:bogus_key: 未知键...`，`rc=1`

```bash
# 例2 缺少必填键（每例前恢复全部三文件，避免前一例改动残留）
cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/ && sed -i '/^  oc_trip_a:/d' /tmp/opencode/params_neg/software.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：software.yaml:fault.oc_trip_a: 缺少必填键`，`rc=1`

```bash
# 例3 未定义符号（每例前恢复全部三文件）
cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/ && sed -i 's|^  vbus_uv_v: 9.0.*|  vbus_uv_v: "9.0 * motor.nonexistent"|' /tmp/opencode/params_neg/software.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：software.yaml:fault.vbus_uv_v: 未定义符号 'motor.nonexistent'...`，`rc=1`

```bash
# 例4 环引用（每例前恢复全部三文件）
cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/ && sed -i 's|^rs_ohm: 0.158.*|rs_ohm: "ls_h * 1.0"|; s|^ls_h: 1.185e-4.*|ls_h: "rs_ohm * 1.0"|' /tmp/opencode/params_neg/motor.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：motor.yaml:rs_ohm: 环引用：motor.rs_ohm -> motor.ls_h -> motor.rs_ohm`，`rc=1`

```bash
# 例5 类型不符（每例前恢复全部三文件）
cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/ && sed -i 's|^  levels: 16.*|  levels: 1.5|' /tmp/opencode/params_neg/hardware.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：hardware.yaml:canid_dip.levels: 必须是整数...`，`rc=1`

```bash
# 例6 跨域越界（超采样满量程；每例前恢复全部三文件）
cp config/motor.yaml config/hardware.yaml config/software.yaml /tmp/opencode/params_neg/ && sed -i 's|^  oc_trip_a: .*|  oc_trip_a: 200.0|' /tmp/opencode/params_neg/software.yaml
python3 scripts/gen_params.py --config-dir /tmp/opencode/params_neg --out-dir /tmp/opencode/params_gen --check; echo "rc=$?"
```
Expected: `[params] 错误：跨域校验: oc_trip_a 超过采样链路满量程（110.0 A）`，`rc=1`

```bash
rm -rf /tmp/opencode/params_neg
```

- [ ] **Step 6: 提交**

```bash
git add scripts/gen_params.py
git commit -m "feat(params): 参数生成器（表达式求值/跨文件引用/模式与跨域校验）"
```

---

## Task 3: 三个 params 模块（类型 + 访问器）

**Files:**
- Create: `App/Control/Inc/app_motor_params.h`, `App/Control/Src/app_motor_params.c`
- Create: `App/Platform/Inc/app_hardware_params.h`, `App/Platform/Src/app_hardware_params.c`
- Create: `App/Platform/Inc/app_software_params.h`, `App/Platform/Src/app_software_params.c`

- [ ] **Step 1: 写入 `App/Control/Inc/app_motor_params.h`**

```c
/*
 * App Motor Params - 电机机械/电磁参数（工厂默认，来源 config/motor.yaml）
 *
 * 消费者：FOC（后续）。YAML 为出厂初值；将来在线辨识结果经 flash 覆盖（load 内叠加）。
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_MOTOR_PARAMS_H
#define APP_MOTOR_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t resolution_bits;    /* 单圈绝对分辨率 [bit] */
    uint8_t rotor_ring_teeth;   /* 转子轴外齿圈齿数（两路小齿轮共用） */
    uint8_t rotor_pinion_teeth; /* 转子编码器小齿轮齿数 */
    uint8_t output_pinion_teeth;/* 出轴编码器小齿轮齿数 */
    float   rotor_ratio;        /* 转子编码器转角/转子转角 */
    float   output_ratio;       /* 出轴编码器转角/转子转角 */
} app_motor_encoder_t;

typedef struct {
    uint8_t  pole_pairs;        /* 极对数 */
    float    rs_ohm;            /* 相电阻 [Ω] */
    float    ls_h;              /* 相电感 [H] */
    float    ke_vs_per_rad;     /* 反电动势系数 [V·s/rad] */
    float    kt_nm_per_a;       /* 转矩系数 [N·m/A] */
    float    i_rated_a;         /* 额定电流 [A]（RMS，105°C） */
    float    i_peak_10s_a;      /* 峰值电流 10s [A]（RMS） */
    float    i_peak_2s_a;       /* 峰值电流 2s [A]（RMS） */
    float    vbus_nom_v;        /* 母线额定电压 [V] */
    uint16_t rpm_max;           /* 最高转速 [rpm] */
    float    inertia_kgm2;      /* 转动惯量 [kg·m²] */
    float    torque_rated_nm;   /* 额定转矩 [N·m] */
    float    torque_peak_10s_nm;/* 峰值转矩 10s [N·m] */
    app_motor_encoder_t encoder;
} app_motor_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_motor_params_t *app_motor_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖（在线辨识结果） */
void app_motor_params_load(app_motor_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_MOTOR_PARAMS_H */
```

- [ ] **Step 2: 写入 `App/Control/Src/app_motor_params.c`**

```c
/*
 * App Motor Params - 电机参数访问器（工厂常量来自 build/generated/params_generated.c）
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_motor_params.h"

#include "params_generated.h"

#include <stddef.h>

const app_motor_params_t *app_motor_params_default(void) {
    return &g_motor_params_factory;
}

void app_motor_params_load(app_motor_params_t *out) {
    if (out == NULL) {
        return;
    }
    *out = g_motor_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_MOTOR, ...)（在线辨识结果） */
}
```

- [ ] **Step 3: 写入 `App/Platform/Inc/app_hardware_params.h`**

```c
/*
 * App HW Params - 硬件电路参数（工厂默认，来源 config/hardware.yaml）
 *
 * 消费者：app_analog_signal / app_adc / app_3phase_inverter / app_fault（标度）。
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_HW_PARAMS_H
#define APP_HW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float shunt_ohm;  /* 采样电阻 [Ω] */
    float amp_gain;   /* 运放增益 */
    float a_per_volt; /* 电流标度 [A/V]（= 1/(shunt×gain)，派生） */
    float bias_v;     /* 零电流偏置 [V]（= vref/2，派生） */
} app_hardware_current_sense_t;

typedef struct {
    uint32_t divider_high_ohm; /* 分压上臂 [Ω] */
    uint32_t divider_low_ohm;  /* 分压下臂 [Ω] */
    float    v_per_volt;       /* 母线标度 [V/V]（派生） */
} app_hardware_vbus_sense_t;

typedef struct {
    uint32_t pullup_ohm; /* 板上上拉 [Ω] */
    float    r25_ohm;    /* NTC 25°C 阻值 [Ω]（占位，待选型） */
    float    b_value_k;  /* B 常数 [K]（占位，待选型） */
    float    max_ohm;    /* 开路/超量程替代值 [Ω] */
} app_hardware_ntc_t;

typedef struct {
    uint8_t levels; /* 拨码档数（占位；解码未实现） */
} app_hardware_canid_t;

typedef struct {
    uint8_t  sample_cycle;    /* 采样窗口 [ADC 时钟数]（SDK 最小 10，勿低于） */
    uint32_t trigger_delay_ns;/* 谷底后触发延时 [ns] */
} app_hardware_adc_t;

typedef struct {
    uint32_t pwm_freq_hz; /* 开关频率 [Hz] */
    uint32_t deadtime_ns; /* HPM 侧死区 [ns] */
} app_hardware_inverter_t;

typedef struct {
    app_hardware_current_sense_t current_sense;
    app_hardware_vbus_sense_t    vbus_sense;
    app_hardware_ntc_t           ntc;
    app_hardware_canid_t         canid_dip;
    app_hardware_adc_t           adc;
    app_hardware_inverter_t      inverter;
} app_hardware_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_hardware_params_t *app_hardware_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖 */
void app_hardware_params_load(app_hardware_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_HW_PARAMS_H */
```

- [ ] **Step 4: 写入 `App/Platform/Src/app_hardware_params.c`**

```c
/*
 * App HW Params - 硬件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_hardware_params.h"

#include "params_generated.h"

#include <stddef.h>

const app_hardware_params_t *app_hardware_params_default(void) {
    return &g_hardware_params_factory;
}

void app_hardware_params_load(app_hardware_params_t *out) {
    if (out == NULL) {
        return;
    }
    *out = g_hardware_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_HARDWARE, ...) */
}
```

- [ ] **Step 5: 写入 `App/Platform/Inc/app_software_params.h`**

```c
/*
 * App SW Params - 软件参数（工厂默认，来源 config/software.yaml）
 *
 * 消费者：app_fault（阈值）/ app_can（波特率）/ FOC（后续，PID 与限幅）。
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_SW_PARAMS_H
#define APP_SW_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t baudrate;        /* CAN 波特率 [bps] */
    uint8_t  node_id_default; /* 默认节点号（占位：DIP 解码未实现；与 CAN ID 的派生关系待协议定稿） */
    uint32_t rx_control_id;   /* 接收控制帧 CAN ID（占位：待协议定稿） */
    uint32_t tx_report_id;    /* 参数回报帧 CAN ID（占位：待协议定稿） */
} app_software_can_t;

typedef struct {
    float    oc_trip_a;     /* 过流阈值 [A] */
    float    vbus_ov_v;     /* 母线过压 [V] */
    float    vbus_uv_v;     /* 母线欠压 [V] */
    uint16_t slow_debounce; /* L2/L3 去抖次数（1kHz） */
    uint16_t adc_stall_ms;  /* PMT 帧停滞超时 [ms] */
    uint8_t  enc_err_delta; /* 编码器错误增量阈值 */
} app_software_fault_t;

typedef struct {
    float kp;
    float ki;
} app_software_pid_t;

typedef struct {
    float i_q_max_a; /* 电流限幅 [A]（RMS 口径；FOC 预留，未消费） */
    float duty_max;  /* 占空比上限（FOC 预留，未消费） */
} app_software_limits_t;

typedef struct {
    app_software_pid_t    current_loop; /* 电流环（FOC 预留） */
    app_software_pid_t    speed_loop;   /* 速度环（FOC 预留） */
    app_software_limits_t limits;
} app_software_control_t;

typedef struct {
    app_software_can_t     can;
    app_software_fault_t   fault;
    app_software_control_t control;
} app_software_params_t;

/** @brief 工厂默认参数（只读，指向生成常量） */
const app_software_params_t *app_software_params_default(void);

/** @brief 加载参数：工厂默认 +（将来）flash 覆盖（整定/自校准结果） */
void app_software_params_load(app_software_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_SW_PARAMS_H */
```

- [ ] **Step 6: 写入 `App/Platform/Src/app_software_params.c`**

```c
/*
 * App SW Params - 软件参数访问器（工厂常量来自 build/generated/params_generated.c）
 * Copyright (c) 2026 HPMicro
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_software_params.h"

#include "params_generated.h"

#include <stddef.h>

const app_software_params_t *app_software_params_default(void) {
    return &g_software_params_factory;
}

void app_software_params_load(app_software_params_t *out) {
    if (out == NULL) {
        return;
    }
    *out = g_software_params_factory;
    /* TODO(v2): flash 覆盖 —— app_param_is_ready() → app_param_load(APP_PARAM_KEY_SOFTWARE, ...)（整定/阈值微调） */
}
```

- [ ] **Step 7: 提交**

```bash
git add App/Control/Inc/app_motor_params.h App/Control/Src/app_motor_params.c \
        App/Platform/Inc/app_hardware_params.h App/Platform/Src/app_hardware_params.c \
        App/Platform/Inc/app_software_params.h App/Platform/Src/app_software_params.c
git commit -m "feat(params): 三域参数模块（类型 + default/load 访问器，flash 覆盖预留）"
```

---

## Task 4: CMake 接入 + 构建验证

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 在 `sdk_link_libraries(m)`（第 29 行）之后插入参数管线块**

```cmake
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
```

- [ ] **Step 2: Platform 段（`app_can.c` 行后）增加两行**

```cmake
sdk_app_src(App/Platform/Src/app_hardware_params.c)
sdk_app_src(App/Platform/Src/app_software_params.c)
```

- [ ] **Step 3: Control 段（`app_fault.c` 行后）增加一行**

```cmake
sdk_app_src(App/Control/Src/app_motor_params.c)
```

- [ ] **Step 4: 重新配置 + 构建**

Run: `make configure > /tmp/opencode/cfg.log 2>&1 && grep "\[params\]" /tmp/opencode/cfg.log; make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -40 /tmp/opencode/build.log`
Expected: configure 输出含 `[params] 生成 51 个参数（motor 19 / hardware 16 / software 16）`；`BUILD OK`

- [ ] **Step 5: 验证生成物与目标文件**

Run: `ls build/generated/ && find build -name "params_generated.c.o" | head -3`
Expected: `params_generated.c  params_generated.h  params_report.txt`；找到 `params_generated.c.o`

- [ ] **Step 6: 提交**

```bash
git add CMakeLists.txt
git commit -m "build(params): configure 期生成参数并加入构建（含 CONFIGURE_DEPENDS）"
```

---

## Task 5: 消费迁移 — `app_fault`（sw 阈值 + hw 标度）

**Files:**
- Modify: `App/Control/Src/app_fault.c`
- Modify: `App/Control/Inc/app_fault.h`

- [ ] **Step 1: 增加 includes + 更新头部注释**

在 `#include "app_encoder.h"` 之后新增两行：

```c
#include "app_hardware_params.h"
#include "app_software_params.h"
```

将 `.c` 头部注释第 8 行：

```c
 * 阈值默认值见 app_fault.h（2026-09-19 评审确认）。
```

改为：

```c
 * 阈值默认值来源：config/software.yaml（经 app_software_params 加载）。
```

- [ ] **Step 2: 删除 `app_fault.h` 6 个默认宏**

将：

```c
/* ============================================================================
 * 默认阈值（2026-09-19 评审确认）
 *   L1/L2 相电流 = 3 × 电机手册峰值 24.3A（G66-18）= 72.9A
 *   L3 母线：OV 36V / UV 9V（24V 系统）
 * ============================================================================ */
#define APP_FAULT_OC_TRIP_A_DEFAULT     (72.9f)
#define APP_FAULT_VBUS_OV_V_DEFAULT     (36.0f)
#define APP_FAULT_VBUS_UV_V_DEFAULT     (9.0f)
#define APP_FAULT_SLOW_DEBOUNCE_DEFAULT (5U)   /* L2/L3 连续次数（1kHz） */
#define APP_FAULT_ADC_STALL_MS_DEFAULT  (10U)  /* PMT 帧停滞 [ms] */
#define APP_FAULT_ENC_ERR_DELTA_DEFAULT (3U)   /* 编码器错误增量 */
#define APP_FAULT_RMS_WINDOW_DEFAULT    (250U) /* 10ms @25kHz */
#define APP_FAULT_SETTLE_TICKS_DEFAULT  (50U)  /* 上电静默期（≈50ms @1kHz） */
```

替换为：

```c
/* ============================================================================
 * 阈值默认值来源：config/software.yaml（app_software_params.fault，app_fault_init 加载）
 * 以下为内部时序量（待 FOC 时序体系定义后接入 YAML，见参数管线设计 §10）
 * ============================================================================ */
#define APP_FAULT_RMS_WINDOW_DEFAULT    (250U) /* 10ms @25kHz */
#define APP_FAULT_SETTLE_TICKS_DEFAULT  (50U)  /* 上电静默期（≈50ms @1kHz） */
```

- [ ] **Step 3: `app_fault_init` 使用参数默认值**

将 `void app_fault_init(const app_fault_cfg_t *cfg) {` 开头的局部变量区：

```c
void app_fault_init(const app_fault_cfg_t *cfg) {
    algo_rms_cfg_t rms_cfg;
    float dev_v;
```

改为：

```c
void app_fault_init(const app_fault_cfg_t *cfg) {
    algo_rms_cfg_t rms_cfg;
    app_hardware_params_t hw;
    app_software_params_t sw;
    float dev_v;
```

并在 `memset(&s_f, 0, sizeof(s_f));` 之后插入：

```c
    /* 工厂默认参数（config/software.yaml + config/hardware.yaml；将来 flash 覆盖） */
    app_hardware_params_load(&hw);
    app_software_params_load(&sw);
```

将 6 处默认值宏替换（`oc_fast_a` / `oc_slow_a` / `vbus_ov_v` / `vbus_uv_v` / `slow_debounce` / `adc_stall_ms` / `enc_err_delta` 的 `: APP_FAULT_*_DEFAULT;` 分支）：

```c
                        : sw.fault.oc_trip_a;      /* oc_fast_a、oc_slow_a 两处 */
                        : sw.fault.vbus_ov_v;
                        : sw.fault.vbus_uv_v;
                            : sw.fault.slow_debounce;
                           : sw.fault.adc_stall_ms;
                            : sw.fault.enc_err_delta;
```

- [ ] **Step 4: WDOG 换算改用 hw 标度**

将：

```c
    dev_v = s_f.oc_fast_a / APP_ANALOG_I_AMP_PER_VOLT;
    dev_cnt = dev_v * (65535.0f / (INTF_ADC_DEFAULT_VREF_MV / 1000.0f));
```

改为：

```c
    dev_v = s_f.oc_fast_a / hw.current_sense.a_per_volt;
    dev_cnt = dev_v * (65535.0f / (INTF_ADC_DEFAULT_VREF_MV / 1000.0f));
```

- [ ] **Step 5: 构建 + 旧引用清零**

Run:
```bash
make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -30 /tmp/opencode/build.log
grep -rn "APP_FAULT_OC_TRIP_A_DEFAULT\|APP_FAULT_VBUS_OV_V_DEFAULT\|APP_FAULT_VBUS_UV_V_DEFAULT\|APP_FAULT_SLOW_DEBOUNCE_DEFAULT\|APP_FAULT_ADC_STALL_MS_DEFAULT\|APP_FAULT_ENC_ERR_DELTA_DEFAULT" App/
grep -n "APP_ANALOG_I_AMP_PER_VOLT" App/Control/Src/app_fault.c
grep -n "INTF_ADC_DEFAULT_VREF_MV" App/Control/Src/app_fault.c
```
Expected: `BUILD OK`；三条 grep 均无输出（`APP_ANALOG_I_AMP_PER_VOLT` 在 `app_analog_signal.h` 的定义与 `app_analog_signal.c` 的使用由 Task 6 处理，Task 5 仅清零 `app_fault.c` 内引用）

- [ ] **Step 6: 提交**

```bash
git add App/Control/Src/app_fault.c App/Control/Inc/app_fault.h
git commit -m "refactor(fault): 阈值与电流标度改从 sw/hw 参数加载（YAML 管线）"
```

---

## Task 6: 消费迁移 — `app_analog_signal` + 删宏

**Files:**
- Modify: `App/Platform/Src/app_analog_signal.c`
- Modify: `App/Platform/Inc/app_analog_signal.h`

- [ ] **Step 1: 头文件 include + 常量块替换 + 残留常量清理**

在 `#include "app_analog_signal.h"` 之后新增 `#include "app_hardware_params.h"`。

将换算常数块及其分区头（第 17~27 行附近）：

```c
/* ============================================================================
 * 换算常数（原理图定值）
 * ============================================================================ */

/* APP_ANALOG_I_AMP_PER_VOLT 定义已上移至头文件（供 app_fault 使用） */
#define APP_ANALOG_VBUS_VOLT_PER_VOLT (22.2121f) /* (15K×4+10K+3.3K)/3.3K */
#define APP_ANALOG_NTC_PULLUP_OHM     (10000.0f)
#define APP_ANALOG_VREF_VOLTS         (INTF_ADC_DEFAULT_VREF_MV / 1000.0f)
#define APP_ANALOG_I_ZERO_VOLTS       (1.65f) /* 上电默认偏置（未标定时） */

/* 零点标定参数 */
```

替换为：

```c
/* ============================================================================
 * 硬件换算参数（来源 config/hardware.yaml，init 时加载）
 * ============================================================================ */

static app_hardware_params_t s_hw;

/* 零点标定过程参数（不随 YAML，标定流程专用） */
```

再删除 `.c` 中独立的 `APP_ANALOG_NTC_MAX_OHM` 宏定义块（该值已由
`hw.ntc.max_ohm` 提供；不删会使引用清零检查失败）：

```c
/* NTC 满量程上限（悬空/超量程时的替代值） */
#define APP_ANALOG_NTC_MAX_OHM (1000000.0f)
```

删除时同步移除该块后的空行，使「零点标定过程参数」块与「滤波器」分区注释之间
保持单空行。

- [ ] **Step 2: 换算函数改用 `s_hw`**

`ntc_resistance_from_volts`：

```c
    const float vs = APP_ANALOG_VREF_VOLTS;
```
→
```c
    const float vs = INTF_ADC_DEFAULT_VREF_MV / 1000.0f;
```

```c
        return APP_ANALOG_NTC_MAX_OHM;
```
→
```c
        return s_hw.ntc.max_ohm;
```

```c
    return APP_ANALOG_NTC_PULLUP_OHM * v / (vs - v);
```
→
```c
    return s_hw.ntc.pullup_ohm * v / (vs - v);
```

`channel_to_physical`：

```c
        return (v - s_zero_volts[ch]) * APP_ANALOG_I_AMP_PER_VOLT;
```
→
```c
        return (v - s_zero_volts[ch]) * s_hw.current_sense.a_per_volt;
```

```c
        return v * APP_ANALOG_VBUS_VOLT_PER_VOLT;
```
→
```c
        return v * s_hw.vbus_sense.v_per_volt;
```

- [ ] **Step 3: `app_analog_signal_init` 加载参数**

```c
void app_analog_signal_init(void) {
    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        s_zero_volts[i] = APP_ANALOG_I_ZERO_VOLTS;
    }
```
→
```c
void app_analog_signal_init(void) {
    app_hardware_params_load(&s_hw); /* config/hardware.yaml（将来 flash 覆盖） */

    for (uint8_t i = 0U; i < APP_ANALOG_CURRENT_COUNT; i++) {
        s_zero_volts[i] = s_hw.current_sense.bias_v;
    }
```

并把滤波器 LPF 的 `.sample_rate_hz = (float) APP_ANALOG_SAMPLE_RATE_HZ,` 改为
`.sample_rate_hz = (float) s_hw.inverter.pwm_freq_hz,`（节拍与开关频率统一，见评审期变更 5）；
删除头文件 `APP_ANALOG_SAMPLE_RATE_HZ` 宏，其注释改为
`/* process() 调用频率 = 主循环节拍（= inverter.pwm_freq_hz，config/hardware.yaml） */`。

- [ ] **Step 4: 删除头文件宏**

`App/Platform/Inc/app_analog_signal.h` 中删除：

```c
/* 电流链路转换常数 [A/V]（TPA6584Q ×7.5，Rshunt 2mΩ → 1/(7.5×2mΩ)） */
#define APP_ANALOG_I_AMP_PER_VOLT (66.6667f)
```

并把文件顶部注释改为符号化公式（系数名对应 `app_hardware_params` 字段，避免写死数字随
YAML 漂移）：

```c
 * 换算（依据原理图 Analog Signal Processing；数值来源 config/hardware.yaml）：
 *   电流   I [A]     = (V_adc − V_zero) × 66.6667      （TPA6584Q ×7.5，Rshunt 2mΩ）
 *   母线   V_bus [V] = V_adc × 22.2121                  （73.3K/3.3K 分压 + 内部运放 B 缓冲）
 *   NTC    R [Ω]     = 10000 × V_adc / (3.3 − V_adc)    （10K 上拉；温度换算待型号确定）
 *
 * 电流链路为比例式：1.65V 偏置与 ADC 基准同源，3.3V 电源漂移不影响精度。
```
→
```c
 * 换算（依据原理图 Analog Signal Processing；系数来源 config/hardware.yaml）：
 *   电流   I [A]     = (V_adc − V_zero) × a_per_volt    （a_per_volt = 1/(shunt×gain)）
 *   母线   V_bus [V] = V_adc × v_per_volt               （v_per_volt = (Rhi+Rlo)/Rlo）
 *   NTC    R [Ω]     = pullup × V_adc / (vref − V_adc)  （pullup = hw.ntc.pullup_ohm；vref = INTF_ADC_DEFAULT_VREF_MV（驱动侧固定 3.3V））
 *
 * 电流链路为比例式：偏置（bias_v）与 ADC 基准同源，3.3V 电源漂移不影响精度。
```

- [ ] **Step 5: 构建 + 全仓引用清零**

Run:
```bash
make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -30 /tmp/opencode/build.log
grep -rn "APP_ANALOG_I_AMP_PER_VOLT\|APP_ANALOG_VBUS_VOLT_PER_VOLT\|APP_ANALOG_NTC_PULLUP_OHM\|APP_ANALOG_NTC_MAX_OHM\|APP_ANALOG_VREF_VOLTS\|APP_ANALOG_I_ZERO_VOLTS" App/ | wc -l
```
Expected: `BUILD OK`；计数 `0`

- [ ] **Step 6: 提交**

```bash
git add App/Platform/Src/app_analog_signal.c App/Platform/Inc/app_analog_signal.h
git commit -m "refactor(analog): 换算常数改从 hw 参数加载，删除 APP_ANALOG_* 宏"
```

---

## Task 7: 消费迁移 — `app_adc` / `app_3phase_inverter` / Debug 打印 + 删宏

**Files:**
- Modify: `App/Platform/Src/app_adc.c`, `App/Platform/Inc/app_adc.h`
- Modify: `App/Platform/Src/app_3phase_inverter.c`, `App/Platform/Inc/app_3phase_inverter.h`
- Modify: `App/Debug/Src/app_debug_inverter.c`, `App/Debug/Src/app_debug_motor.c`

- [ ] **Step 1: `app_adc.c` 默认值改从 hw 参数**

include 区新增 `#include "app_hardware_params.h"`（放在 `#include "app_adc.h"` 之后）。

`app_adc_init` 局部变量区新增 `app_hardware_params_t hw;`，并在 `s_cfg = (app_adc_cfg_t) {` 之前插入：

```c
    app_hardware_params_load(&hw); /* config/hardware.yaml（将来 flash 覆盖） */
```

将：

```c
        .trigger_delay_ns = APP_ADC_TRIGGER_DELAY_NS_DEFAULT,
        .sample_cycle = APP_ADC_SAMPLE_CYCLE_DEFAULT,
```
→
```c
        .trigger_delay_ns = hw.adc.trigger_delay_ns,
        .sample_cycle = hw.adc.sample_cycle,
```

将两处回退分支：

```c
            s_cfg.sample_cycle = APP_ADC_SAMPLE_CYCLE_DEFAULT;
```
→
```c
            s_cfg.sample_cycle = hw.adc.sample_cycle;
```

```c
            s_cfg.trigger_delay_ns = APP_ADC_TRIGGER_DELAY_NS_DEFAULT;
```
→
```c
            s_cfg.trigger_delay_ns = hw.adc.trigger_delay_ns;
```

- [ ] **Step 2: `app_adc.h` 删除默认宏**

删除：

```c
/* 默认配置（后续由 YAML 参数管线提供） */
#define APP_ADC_TRIGGER_DELAY_NS_DEFAULT (500U) /* 谷底后触发延时 [ns] */
#define APP_ADC_SAMPLE_CYCLE_DEFAULT     (25U)  /* 采样窗口 [ADC 时钟数]：对齐模板/原工程默认值
                                                * （SDK 最小值 10 曾在多工程复现"通道数据重复"，
                                                *   FOC 示例用 20，原工程用 25） */
```

替换为：

```c
/* 默认配置来源：config/hardware.yaml（app_hardware_params.adc） */
```

- [ ] **Step 3: `app_3phase_inverter.c` 默认值改从 hw 参数**

include 区新增 `#include "app_hardware_params.h"`。

`app_3phase_inverter_init`：

```c
void app_3phase_inverter_init(const app_3phase_inverter_cfg_t *cfg)
{
    app_3phase_inverter_cfg_t c = {
        .pwm_freq_hz = APP_3PHASE_INVERTER_FREQ_HZ_DEFAULT,
        .deadtime_ns = APP_3PHASE_INVERTER_DEADTIME_NS_DEFAULT,
    };

    if (cfg != NULL) {
        c = *cfg;
    }
```
→
```c
void app_3phase_inverter_init(const app_3phase_inverter_cfg_t *cfg)
{
    app_hardware_params_t hw;
    app_3phase_inverter_cfg_t c;

    app_hardware_params_load(&hw); /* config/hardware.yaml（将来 flash 覆盖） */
    c = (app_3phase_inverter_cfg_t) {
        .pwm_freq_hz = hw.inverter.pwm_freq_hz,
        .deadtime_ns = hw.inverter.deadtime_ns,
    };

    if (cfg != NULL) {
        c = *cfg;
    }
```

并将注释 `/* 按配置重配三相（频率/死区；后续由 YAML 参数管线提供） */` 改为
`/* 按配置重配三相（频率/死区；来源 config/hardware.yaml） */`。

- [ ] **Step 4: `app_3phase_inverter.h` 删宏 + 注释更新**

删除：

```c
/*
 * 初始化配置（后续由 YAML 参数管线提供，见 config/motor.yaml → motor_params）
 *   默认值用于参数管线接入前；更换 MOS/驱动电路后调整 deadtime_ns
 */
#define APP_3PHASE_INVERTER_FREQ_HZ_DEFAULT     (25000U) /* 开关频率 [Hz] */
#define APP_3PHASE_INVERTER_DEADTIME_NS_DEFAULT (50U)    /* HPM 侧死区 [ns] */
```

替换为：

```c
/*
 * 初始化配置（默认值来源 config/hardware.yaml → app_hardware_params.inverter）
 *   更换 MOS/驱动电路后调整 YAML 中的 deadtime_ns
 */
```

同时把结构体字段注释 `（YAML: inverter.pwm_freq_hz）` / `（YAML: inverter.deadtime_ns）` 保留（仍准确）。

- [ ] **Step 5: `app_debug_inverter.c` 打印改读 hw 参数**

include 区新增 `#include "app_hardware_params.h"`。

```c
void app_debug_inverter_init(void)
{
#if INVERTER_TEST_ENABLE
    app_debug_printf("\r\n[3PH] 三相逆变桥输出自检：%u Hz / %u%% / 持续\r\n",
                     (unsigned) APP_3PHASE_INVERTER_FREQ_HZ_DEFAULT,
                     (unsigned) (INVERTER_TEST_DUTY * 100.0f));
```
→
```c
void app_debug_inverter_init(void)
{
#if INVERTER_TEST_ENABLE
    app_hardware_params_t hw;

    app_hardware_params_load(&hw); /* config/hardware.yaml */
    app_debug_printf("\r\n[3PH] 三相逆变桥输出自检：%u Hz / %u%% / 持续\r\n",
                     (unsigned) hw.inverter.pwm_freq_hz,
                     (unsigned) (INVERTER_TEST_DUTY * 100.0f));
```

- [ ] **Step 6: `app_debug_motor.c` 触发延时复位改读 hw 参数**

include 区新增 `#include "app_hardware_params.h"`。

```c
    (void) app_adc_set_trigger_delay_ns(APP_ADC_TRIGGER_DELAY_NS_DEFAULT);
```
→
```c
    {
        app_hardware_params_t hw;

        app_hardware_params_load(&hw); /* config/hardware.yaml */
        (void) app_adc_set_trigger_delay_ns(hw.adc.trigger_delay_ns);
    }
```

- [ ] **Step 7: 构建 + 全仓引用清零**

Run:
```bash
make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -30 /tmp/opencode/build.log
grep -rn "APP_ADC_TRIGGER_DELAY_NS_DEFAULT\|APP_ADC_SAMPLE_CYCLE_DEFAULT\|APP_3PHASE_INVERTER_FREQ_HZ_DEFAULT\|APP_3PHASE_INVERTER_DEADTIME_NS_DEFAULT" App/ | wc -l
```
Expected: `BUILD OK`；计数 `0`

- [ ] **Step 7b: 注释收口（质量评审批次，纯文档）**

消费迁移完成后，残留以下 5 处写死/悬空注释，逐项修正（不改功能）：

1. `App/Platform/Inc/app_3phase_inverter.h`
   `@param ... NULL = 使用默认值（APP_3PHASE_INVERTER_*_DEFAULT）`
   → `@param ... NULL = 使用默认值（config/hardware.yaml → app_hardware_params.inverter）`（悬空宏引用）
2. `App/Platform/Inc/app_adc.h` doxygen
   `@param cfg 配置；NULL = 默认（500ns 延时、16bit、sample_cycle=10）`
   → `@param cfg 配置；NULL = 默认（trigger_delay/sample_cycle 取自 config/hardware.yaml；resolution=16bit）`
   （原注释的 sample_cycle=10 与实际默认 25 不符）
3. `App/Platform/Inc/app_adc.h` 字段注释
   `uint32_t trigger_delay_ns; /* ...（0 = 默认 500ns） */`
   → `/* ...（0 = 回退 hw.adc.trigger_delay_ns） */`（去写死数值）
4. `App/Platform/Inc/app_adc.h` 头部时序说明
   `时序（25kHz / MOT 160MHz / ADC 40MHz，16bit + sample_cycle=25）：`
   → `时序（按当前默认配置：25kHz / MOT 160MHz / ADC 40MHz，16bit + sample_cycle=25）：`（限定"当前默认"）
5. `App/Debug/Src/app_debug_inverter.c` 头部说明
   `测试内容：三相 25kHz 中心对齐、...`
   → `测试内容：三相默认频率（见 config/hardware.yaml）中心对齐、...`（去写死频率）

验证：`make build` rc=0 且 warning/error 计数 0；
`grep -rn "APP_3PHASE_INVERTER_\*_DEFAULT\|sample_cycle=10\|0 = 默认 500ns" App/` 无输出。

- [ ] **Step 8: 提交**

```bash
git add App/Platform/Src/app_adc.c App/Platform/Inc/app_adc.h \
        App/Platform/Src/app_3phase_inverter.c App/Platform/Inc/app_3phase_inverter.h \
        App/Debug/Src/app_debug_inverter.c App/Debug/Src/app_debug_motor.c
git commit -m "refactor(adc,inverter): 默认值改从 hw 参数加载，删除 APP_ADC_*/APP_3PHASE_INVERTER_* 宏"
```

---

## Task 8: 消费迁移 — `app_can` + 删宏

**Files:**
- Modify: `App/Platform/Src/app_can.c`

- [ ] **Step 1: 删除波特率宏 + 加载 sw 参数**

```c
/* MCAN3: PA15(MCAN3_TXD) / PA14(MCAN3_RXD)，与板级 pinmux 对应 */
#define APP_CAN_INST      (3U)
#define APP_CAN_BAUDRATE  (1000000U)
```
→
```c
/* MCAN3: PA15(MCAN3_TXD) / PA14(MCAN3_RXD)，与板级 pinmux 对应 */
#define APP_CAN_INST      (3U)
```

include 区新增 `#include "app_software_params.h"`。

`app_can_init` 内（`app_can_reset_state();` 之后）：

```c
    {
        intf_can_cfg_t cfg = {
            .baudrate     = APP_CAN_BAUDRATE,
```
→
```c
    {
        app_software_params_t sw;

        app_software_params_load(&sw); /* config/software.yaml（将来 flash 覆盖） */
        intf_can_cfg_t cfg = {
            .baudrate     = sw.can.baudrate,
```

- [ ] **Step 2: 构建 + 引用清零**

Run:
```bash
make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -30 /tmp/opencode/build.log
grep -rn "APP_CAN_BAUDRATE" App/ | wc -l
```
Expected: `BUILD OK`；计数 `0`

- [ ] **Step 3: 提交**

```bash
git add App/Platform/Src/app_can.c
git commit -m "refactor(can): 波特率改从 sw 参数加载，删除 APP_CAN_BAUDRATE 宏"
```

---

## Task 9: 启动摘要打印 + 文档收尾

**Files:**
- Modify: `App/Logic/app_logic.c`
- Modify: `App/Debug/Inc/app_debug_can.h`
- Modify: `docs/superpowers/specs/2026-09-18-m1-board-bringup-design.md`
- Modify: `README.md`

- [ ] **Step 1: `app_logic.c` 增加 includes + 启动摘要**

include 区（`#include "app_fault.h"` 附近）新增：

```c
#include "app_hardware_params.h"
#include "app_motor_params.h"
#include "app_software_params.h"
```

在 `app_init()` 的 boot 打印块之后（`rst_status` 打印结束后）插入：

```c
    /* 0b. 参数摘要（验证 YAML 参数管线端到端：config/{motor,hardware,software}.yaml → 生成 → 加载） */
    {
        app_motor_params_t mp;
        app_hardware_params_t hw;
        app_software_params_t sw;

        app_motor_params_load(&mp);
        app_hardware_params_load(&hw);
        app_software_params_load(&sw);
        app_debug_printf(
            "params: pp=%u rs=%.4f ls=%g | a/v=%.4f vbus/v=%.4f | oc=%.1f ov=%.1f uv=%.1f | pwm=%u/%u\r\n",
            (unsigned) mp.pole_pairs, (double) mp.rs_ohm, (double) mp.ls_h,
            (double) hw.current_sense.a_per_volt, (double) hw.vbus_sense.v_per_volt,
            (double) sw.fault.oc_trip_a, (double) sw.fault.vbus_ov_v, (double) sw.fault.vbus_uv_v,
            (unsigned) hw.inverter.pwm_freq_hz, (unsigned) hw.inverter.deadtime_ns);
    }
```

同时将 `app_run()` 的控制节拍改为由 hw 参数派生（删除 `#define APP_LOOP_FREQ_HZ (25000U)`；
`slow_cycles` / `hb_cycles` 仍用 `cpu_freq`）：

```c
void app_run(void) {
    app_hardware_params_t hw;
    uint32_t cpu_freq;
    uint32_t loop_cycles;

    app_hardware_params_load(&hw); /* 控制节拍 = 半桥开关频率（config/hardware.yaml） */
    cpu_freq = intf_clock_get_cpu_freq();
    loop_cycles = cpu_freq / hw.inverter.pwm_freq_hz;

    const uint32_t slow_cycles = (cpu_freq / 1000U) * APP_SLOW_TASK_PERIOD_MS;
    const uint32_t hb_cycles = (cpu_freq / 1000U) * APP_HEARTBEAT_INTERVAL_MS;
    uint32_t next = intf_clock_get_cycle() + loop_cycles;
    ...
```

- [ ] **Step 2: 构建验证**

Run: `make build > /tmp/opencode/build.log 2>&1 && echo "BUILD OK" || tail -30 /tmp/opencode/build.log; make artifacts > /dev/null 2>&1 && ls -la output/HPM53M1_G6618Motor.elf`
Expected: `BUILD OK`；产物时间戳更新

- [ ] **Step 3: bringup §6.4 加取代注记**

在 `docs/superpowers/specs/2026-09-18-m1-board-bringup-design.md` 的
`### 6.4 YAML 参数管线（方案 B）` 标题之后插入：

```markdown
> **注（2026-09-20）**：本节方案已被 `2026-09-20-param-pipeline-design.md` 取代——
> 单文件 `motor.yaml` 改为三域三文件（motor/hardware/software），宏方案改为类型化
> 结构体 + 访问器（方案 B），并支持表达式与跨文件符号引用。
```

- [ ] **Step 4: README 注明构建依赖**

在 `README.md` 的构建说明区域追加：

```markdown
> 参数管线依赖：构建期执行 `scripts/gen_params.py`，需要 `python3` + `PyYAML`
> （环境仓库已具备 6.0.3；脱离工作区构建时需自行安装）。
```

- [ ] **Step 5: 前序评审遗留文档一致性**

`app_logic.c` 第 5 步 CAN 自检注释：`经典 CAN @1Mbps` →
`经典 CAN；总线波特率与周期帧 ID 来源 config/software.yaml`。

`App/Debug/Inc/app_debug_can.h` 文件头说明：`以经典 CAN 模式初始化 MCAN3（1Mbps）` →
`以经典 CAN 模式初始化 MCAN3（正常运行波特率来源 config/software.yaml；环回自检固定 1Mbps）`。

`App/Debug/Inc/app_debug_can.h` 周期任务说明：周期帧 ID 改为
`ID 取自 config/software.yaml → sw.can.tx_report_id`（8 字节，首字节为递增计数）。

- [ ] **Step 6: 提交**

```bash
git add App/Logic/app_logic.c App/Debug/Inc/app_debug_can.h \
        docs/superpowers/specs/2026-09-18-m1-board-bringup-design.md README.md
git commit -m "feat(params): 启动参数摘要打印 + 文档收尾（§6.4 取代注记、README 依赖说明）"
```

---

## Task 10: 台架验证（需 Kaiser 操作硬件）

**Files:** 无代码改动；验证清单。

- [ ] **Step 1: 烧录 + 观察启动摘要**

烧录 `output/HPM53M1_G6618Motor.elf`（外部电源供电），RTT 观察。
Expected（与 YAML 一致）：
```text
params: pp=10 rs=0.1580 ls=0.0001185 | a/v=66.6667 vbus/v=22.2121 | oc=72.9 ov=36.0 uv=9.0 | pwm=25000/50
```

- [ ] **Step 2: 既有功能回归**

- `f`：阈值默认不变（72.9 / 36 / 9 / 5 / 10 / 3），状态 NORMAL；
- `d`：母线 ≈23.7V、电流零点 ≈0A、NTC 高阻（与迁移前一致）；
- 逆变器自检打印 25kHz；
- ADC 序列（1kHz）与 PMT（25kHz）节拍正常（`p` 寄存器行）。

- [ ] **Step 3: 故障链路复测（欠压）**

手动降母线 → 检出（去抖 5 次）→ 锁存 + 快照 → 恢复 → `F` 清除全归零。
Expected：与迁移前行为一致（阈值来源改变，逻辑不变）。

- [ ] **Step 4: 参数改动冒烟（可选）**

改 `config/software.yaml` 的 `slow_debounce: 5` → `3`，`make build` 后确认
`build/generated/params_report.txt` 与 elf 启动摘要同步更新。

---

## 验证与回滚

- 每 Task 构建通过 + 旧宏引用清零（grep 计数 = 0）；
- 回滚：按 Task 粒度 `git revert`（Task 1-9 各自独立提交）；
- 生成器负例 6 项（Task 2 Step 5）覆盖：未知键/缺键/未定义符号/环引用/类型/跨域越界。

---

## 评审期变更（2026-09-20，Kaiser 评审反馈）

1. **默认 CAN ID 字段加入**：`software.yaml` can 节新增 `rx_control_id` / `tx_report_id`
   （占位；十六进制表达式保留溯源）；SCHEMA、`app_software_can_t`、spec/plan 同步；参数总数 50 → 52。
2. **CAN 自检周期帧接线**：`app_debug_can.c` 的 1Hz 总线发送帧 ID 由旧自检常量 (0x114)
   改为 `sw.can.tx_report_id`；init 打印改为输出总线实际波特率与回报 ID；
   环回自检保留内部常量（测试隔离）；`rx_control_id` 暂无消费者（待协议逻辑）。
3. **CAN ID 范围收窄**：`tx_report_id`/`rx_control_id` SCHEMA 上限由 0x1FFFFFFF 收窄为 0x7FF
   （标准帧；构建期拒绝扩展帧，待协议定义帧格式字段后放宽）；自检周期帧 ID 改为 init 缓存；
   环回自检常量改名 `CAN_LB_ID`/`CAN_LB_BAUDRATE`；相关注释同步。
4. **vref 移出 YAML**（Kaiser 决策）：ADC 基准为驱动侧固定 3.3V（硬件 VREF 接 3.3V 输出），
   不进 YAML；`bias_v` 改直接值；analog/fault 换算回退 `INTF_ADC_DEFAULT_VREF_MV` 单一来源。
5. **节拍与开关频率统一**：主循环节拍（app_logic）与模拟量采样率（app_analog_signal）
   改由 `inverter.pwm_freq_hz` 派生；`APP_LOOP_FREQ_HZ`/`APP_ANALOG_SAMPLE_RATE_HZ` 宏删除。
6. **Minor 清理**：消费者注释补全、sample_cycle 注释、app_debug_can 常量命名统一、回报帧负载占位注记；节拍相关常量注释加 TBD 标注（FOC 时序接入后派生）。
7. **生成器增强（B5/B6/B4）**：错误消息加 YAML 行号（`文件:行:键`）；CAN ID 字段 hex 输出
   （`HEX_FIELDS`），YAML 改回无引号十六进制字面量；新增 `scripts/test_gen_params.py` 自测（13 用例，含 production-config-check）；测试夹具 scripts/tests/fixtures/ 与生产配置解耦；重复键错误统一为 文件:行:键。

