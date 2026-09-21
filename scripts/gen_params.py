#!/usr/bin/env python3
"""参数管线生成器：config/*.yaml（三域）→ 生成物（均不含时间戳，可复现构建）

用法：
    python3 scripts/gen_params.py --config-dir config --out-dir build/generated [--check]

产物：
    params_generated.{h,c}       工厂默认参数常量（类型化结构体）
    params_meta_generated.{h,c}  参数元数据表（Shell param 命令与补全用；offset/type/domain/apply/unit）
    params_report.txt            人读参数报告

- 表达式：引号字符串按计算式求值（构建期）；引用支持 同文件叶子名 / 节.字段 / 跨文件 域.路径
- 校验：模式（必填/未知键/类型/范围）+ 跨域一致性；任一失败 → 非零退出 + 文件:行:键:原因
- apply：启动期消费项在 APPLY_REBOOT 集合声明（缺省 live）；unit 从 YAML 行注释 [..] 尽力提取
"""
from __future__ import annotations

import argparse
import ast
import math
import re
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
        ("encoder.electrical_offset_rad", "encoder.electrical_offset_rad", "f32", 0.0, 6.283185307179586),
        ("encoder.direction", "encoder.direction", "f32", -1.0, 1.0),
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
        ("control.current_loop.bandwidth_rad_s", "control.current_loop.bandwidth_rad_s", "f32", 0.0, None),
        ("control.current_loop.decoupling_en", "control.current_loop.decoupling_en", "u8", 0, 1),
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

# 启动期消费项：运行期 set 仅改 RAM，重启后生效；其余缺省 live。键为 "域.路径"
# （与 HEX_FIELDS 同模式：属性在生成器内声明，YAML 保持不变）
# 审计基准（2026-09-21）：被模块 init 期缓存消费的字段 → reboot；
# 逐次使用读取（模拟量换算/NTC）或无消费者（motor/control，FOC 后续）→ live。
APPLY_REBOOT = {
    # 硬件：init 期缓存（ADC 链 / PWM / 逆变器）
    "hardware.adc.sample_cycle",
    "hardware.adc.trigger_delay_ns",
    "hardware.inverter.pwm_freq_hz",
    "hardware.inverter.deadtime_ns",
    # 硬件：派生输入（仅通过 init 期换算值生效）
    "hardware.current_sense.shunt_ohm",
    "hardware.current_sense.amp_gain",
    "hardware.current_sense.bias_v",
    "hardware.vbus_sense.divider_high_ohm",
    "hardware.vbus_sense.divider_low_ohm",
    "hardware.canid_dip.levels",
    # 软件：CAN init 期配置
    "software.can.baudrate",
    "software.can.node_id_default",
    "software.can.rx_control_id",
    "software.can.tx_report_id",
    # 软件：保护阈值（init 期换算为 WDOG 寄存器值/RMS 阈值）
    "software.fault.oc_trip_a",
    "software.fault.vbus_ov_v",
    "software.fault.vbus_uv_v",
    "software.fault.slow_debounce",
    "software.fault.adc_stall_ms",
    "software.fault.enc_err_delta",
}

META_DOMAIN_ENUMS = {
    "motor": "PARAM_META_DOMAIN_MOTOR",
    "hardware": "PARAM_META_DOMAIN_HARDWARE",
    "software": "PARAM_META_DOMAIN_SOFTWARE",
}
META_TYPE_ENUMS = {
    "u8": "PARAM_META_TYPE_U8",
    "u16": "PARAM_META_TYPE_U16",
    "u32": "PARAM_META_TYPE_U32",
    "f32": "PARAM_META_TYPE_F32",
}
META_APPLY_LIVE = "PARAM_META_APPLY_LIVE"
META_APPLY_REBOOT = "PARAM_META_APPLY_REBOOT"

# YAML 行注释中的单位标记（如 "# ... [Ω]"）；取最后一个匹配
_UNIT_RE = re.compile(r"\[([^\]\[]+)\]")


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


def extract_units(config_dir: Path, all_marks) -> dict:
    """尽力从 YAML 源码行注释中提取单位（取最后一个 [..]）；失败则缺省（NULL），不报错。"""
    units = {d: {} for d in DOMAINS}
    for d in DOMAINS:
        marks = all_marks[d]
        if not marks:
            continue
        try:
            lines = (config_dir / FILES[d]).read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for path, mark in marks.items():
            line_no = mark[0]
            if not (1 <= line_no <= len(lines)):
                continue
            matches = _UNIT_RE.findall(lines[line_no - 1])
            if matches:
                unit = matches[-1].strip()
                if unit:
                    units[d][path] = unit
    return units


def c_string(s: str) -> str:
    """转义为 C 字符串字面量（单位可能含非 ASCII 字符，UTF-8 直出）。"""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_meta_header() -> str:
    return """/*
 * 自动生成 — 请勿手改！
 * 来源：config/motor.yaml / config/hardware.yaml / config/software.yaml
 * 生成器：scripts/gen_params.py（构建期执行；不含时间戳，可复现）
 * 用途：Shell param 命令与参数名补全的运行时元数据表
 */
#ifndef PARAMS_META_GENERATED_H
#define PARAMS_META_GENERATED_H

#include <stdint.h>

#include "app_motor_params.h"
#include "app_hardware_params.h"
#include "app_software_params.h"

/** @brief 参数所属域 */
typedef enum {
    PARAM_META_DOMAIN_MOTOR = 0, /**< 电机参数域 */
    PARAM_META_DOMAIN_HARDWARE,  /**< 硬件参数域 */
    PARAM_META_DOMAIN_SOFTWARE,  /**< 软件参数域 */
} param_meta_domain_t;

/** @brief 参数存储类型 */
typedef enum {
    PARAM_META_TYPE_U8 = 0, /**< uint8_t */
    PARAM_META_TYPE_U16,    /**< uint16_t */
    PARAM_META_TYPE_U32,    /**< uint32_t */
    PARAM_META_TYPE_F32,    /**< float */
} param_meta_type_t;

/** @brief 参数生效方式 */
typedef enum {
    PARAM_META_APPLY_LIVE = 0, /**< set 立即生效（消费者实时读取） */
    PARAM_META_APPLY_REBOOT,   /**< set 仅改 RAM，重启后生效 */
} param_meta_apply_t;

/**
 * @brief 单个参数的运行时元数据
 */
typedef struct {
    const char *name;   /**< 域限定名（"<域>.<路径>"，如 hardware.current_sense.amp_gain） */
    uint16_t offset;    /**< 所属域结构体内偏移（offsetof） */
    uint8_t type;       /**< param_meta_type_t */
    uint8_t domain;     /**< param_meta_domain_t */
    uint8_t apply;      /**< param_meta_apply_t */
    float min;          /**< 允许下限（-INFINITY = 无下限） */
    float max;          /**< 允许上限（INFINITY = 无上限） */
    const char *unit;   /**< 单位（可为 NULL） */
} param_meta_t;

extern const param_meta_t g_params_meta[];  /**< 参数元数据表（motor → hardware → software） */
extern const uint32_t g_params_meta_count;  /**< 表项数量 */

#endif /* PARAMS_META_GENERATED_H */
"""


def _fmt_bound(value, fallback: str) -> str:
    """数值边界 → C 字面量；None → fallback（±INFINITY）。"""
    if value is None:
        return fallback
    return f"{float(value)}f"


def gen_meta_source(units) -> str:
    lines = [
        '#include "params_meta_generated.h"',
        "",
        "#include <math.h>",
        "#include <stddef.h>",
        "",
        "const param_meta_t g_params_meta[] = {",
    ]
    for d in DOMAINS:
        for path, cfield, ctype, lo, hi in SCHEMA[d]:
            apply = META_APPLY_REBOOT if f"{d}.{path}" in APPLY_REBOOT else META_APPLY_LIVE
            unit = units[d].get(path)
            unit_lit = "NULL" if unit is None else c_string(unit)
            min_lit = _fmt_bound(lo, "-INFINITY")
            max_lit = _fmt_bound(hi, "INFINITY")
            lines.append(
                f'    {{"{d}.{path}", offsetof({STRUCT_NAMES[d]}, {cfield}), '
                f'{META_TYPE_ENUMS[ctype]}, {META_DOMAIN_ENUMS[d]}, {apply}, '
                f'{min_lit}, {max_lit}, {unit_lit}}},'
            )
    lines.append("};")
    lines.append("")
    lines.append("const uint32_t g_params_meta_count = sizeof(g_params_meta) / sizeof(g_params_meta[0]);")
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
    units = extract_units(args.config_dir, all_marks)
    write_if_changed(args.out_dir / "params_generated.h", gen_header())
    write_if_changed(args.out_dir / "params_generated.c", gen_source(symbols))
    write_if_changed(args.out_dir / "params_report.txt", gen_report(symbols, flats))
    write_if_changed(args.out_dir / "params_meta_generated.h", gen_meta_header())
    write_if_changed(args.out_dir / "params_meta_generated.c", gen_meta_source(units))
    n = sum(len(symbols[d]) for d in DOMAINS)
    print(f"[params] 生成 {n} 个参数"
          f"（motor {len(symbols['motor'])} / hardware {len(symbols['hardware'])} / software {len(symbols['software'])}）"
          f" + 元数据表 {n} 项 → {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
