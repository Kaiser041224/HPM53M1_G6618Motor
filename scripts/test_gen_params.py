#!/usr/bin/env python3
"""生成器自测：scripts/test_gen_params.py

用法：
    python3 scripts/test_gen_params.py

变异用例以 `scripts/tests/fixtures/` 的 YAML 夹具为基准（与生产 `config/` 解耦），
复制到临时目录后做正则变异，再调用 gen_params.main() 断言返回码与 stderr/stdout
消息（用 contextlib.redirect_* 捕获）。另含一个 `production-config-check` 用例对
真实 `config/` 仅跑 `--check`。全部通过退出码 0；任一失败打印汇总并以退出码 1 结束。
仅依赖标准库 + PyYAML。
"""
import sys

sys.dont_write_bytecode = True

import contextlib
import importlib.util
import io
import re
import shutil
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CONFIG = ROOT / "config"
FIXTURES = HERE / "tests" / "fixtures"
FILES = ("motor.yaml", "hardware.yaml", "software.yaml")

RESULTS = []


def _load_gen():
    spec = importlib.util.spec_from_file_location("gen_params", HERE / "gen_params.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = _load_gen()


def expect(name: str, cond: bool, detail: str = "") -> None:
    RESULTS.append((name, bool(cond)))
    print(f"{'PASS' if cond else 'FAIL'} {name}" + ("" if cond else f": {detail}"))


def fresh_copy(root: Path) -> Path:
    d = Path(tempfile.mkdtemp(prefix="case_", dir=root))
    for n in FILES:
        shutil.copy(FIXTURES / n, d / n)
    return d


def run(cfg_dir: Path, check: bool):
    argv = ["--config-dir", str(cfg_dir), "--out-dir", str(cfg_dir / "out")]
    if check:
        argv.append("--check")
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = gen.main(argv)
    return rc, out.getvalue(), err.getvalue()


META_ENTRY_RE = re.compile(
    r'\{"(?P<name>[^"]*)",\s*'
    r'offsetof\((?P<struct>\w+),\s*(?P<cfield>[\w.]+)\),\s*'
    r'(?P<type>PARAM_META_TYPE_\w+),\s*'
    r'(?P<domain>PARAM_META_DOMAIN_\w+),\s*'
    r'(?P<apply>PARAM_META_APPLY_\w+),\s*'
    r'(?P<min>-?INFINITY|-?[\d.]+)f?,\s*'
    r'(?P<max>-?INFINITY|-?[\d.]+)f?,\s*'
    r'(?P<unit>NULL|"[^"]*")\}'
)


def parse_meta(cfg: Path) -> list:
    src = (cfg / "out" / "params_meta_generated.c").read_text(encoding="utf-8")
    return [m.groupdict() for m in META_ENTRY_RE.finditer(src)]


def schema_items() -> list:
    return [(d, path, cfield, ctype)
            for d in gen.DOMAINS
            for (path, cfield, ctype, _lo, _hi) in gen.SCHEMA[d]]


def sub1(path: Path, pattern: str, repl: str) -> None:
    """正则替换且断言恰好命中 1 处（避免硬编码字面值 / 静默失配）。"""
    text = path.read_text(encoding="utf-8")
    new, n = re.subn(pattern, repl, text, flags=re.M)
    if n != 1:
        raise AssertionError(f"期望恰好 1 处替换，实际 {n}：{pattern!r} @ {path.name}")
    path.write_text(new, encoding="utf-8")


def append(path: Path, line: str) -> None:
    with path.open("a", encoding="utf-8") as f:
        f.write(line)


# --------------------------------------------------------------------------- #
# 用例
# --------------------------------------------------------------------------- #
def test_normal(root: Path) -> None:
    cfg = fresh_copy(root)
    rc, so, se = run(cfg, check=False)
    report = (cfg / "out" / "params_report.txt").read_text(encoding="utf-8")
    src = (cfg / "out" / "params_generated.c").read_text(encoding="utf-8")
    ok = (
        rc == 0
        and "a_per_volt = 66.6666667" in report
        and "bias_v = 1.65" in report
        and "oc_trip_a = 72.9" in report
        and "tx_report_id = 0x181" in report
        and "0x101U" in src
        and "0x181U" in src
    )
    expect("normal-config", ok, f"rc={rc} se={se!r} so={so!r}")


def test_production_config_check(root: Path) -> None:
    rc, so, se = run(CONFIG, check=True)
    expect("production-config-check", rc == 0 and "校验通过" in so, f"rc={rc} se={se!r}")


def test_motor_kt_flux_consistency(root: Path) -> None:
    cfg = fresh_copy(root)
    rc, so, se = run(cfg, check=False)
    values = {}
    for line in (cfg / "out" / "params_report.txt").read_text(encoding="utf-8").splitlines():
        if " = " not in line or line.startswith("#"):
            continue
        key, raw = line.split(" = ", 1)
        try:
            values[key] = float(raw.split("#", 1)[0].strip())
        except ValueError:
            continue  # 仅文档字段（如 motor.name）
    pole_pairs = values["motor.pole_pairs"]
    kt = values["motor.kt_nm_per_a"]
    flux = values["motor.flux_linkage_wb"]
    expected = kt / (1.5 * pole_pairs)
    expect(
        "motor-kt-flux-consistency",
        rc == 0 and abs(flux - expected) < 1e-6,
        f"rc={rc} flux={flux} kt={kt} expected={expected} se={se!r}",
    )


def test_unknown_key(root: Path) -> None:
    cfg = fresh_copy(root)
    append(cfg / "motor.yaml", "bogus_key: 1\n")
    rc, so, se = run(cfg, check=True)
    ok = (
        rc == 1
        and re.search(r"motor\.yaml:\d+:", se) is not None
        and "未知键" in se
        and "Traceback" not in se
    )
    expect("unknown-key-with-line-number", ok, f"rc={rc} se={se!r}")


def test_missing_required(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^  oc_trip_a:.*\n", "")
    rc, so, se = run(cfg, check=True)
    expect("missing-required-key", rc == 1 and "缺少必填键" in se, f"rc={rc} se={se!r}")


def test_undefined_symbol(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^  vbus_uv_v:.*$", '  vbus_uv_v: "9.0 * motor.nonexistent"')
    rc, so, se = run(cfg, check=True)
    expect("undefined-symbol", rc == 1 and "未定义符号" in se, f"rc={rc} se={se!r}")


def test_cycle(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "motor.yaml", r"^rs_ohm:.*$", 'rs_ohm: "ls_h * 1.0"')
    sub1(cfg / "motor.yaml", r"^ls_h:.*$", 'ls_h: "rs_ohm * 1.0"')
    rc, so, se = run(cfg, check=True)
    expect("cycle-reference", rc == 1 and "环引用" in se, f"rc={rc} se={se!r}")


def test_type_mismatch(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "hardware.yaml", r"^  levels:.*$", "  levels: 1.5")
    rc, so, se = run(cfg, check=True)
    expect("type-mismatch", rc == 1 and "必须是整数" in se, f"rc={rc} se={se!r}")


def test_out_of_range(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^  rx_control_id:.*$", "  rx_control_id: 0x800")
    rc, so, se = run(cfg, check=True)
    expect("out-of-range", rc == 1 and "越界" in se, f"rc={rc} se={se!r}")


def test_duplicate_key(root: Path) -> None:
    cfg = fresh_copy(root)
    append(cfg / "motor.yaml", "pole_pairs: 12\n")
    rc, so, se = run(cfg, check=True)
    ok = rc == 1 and re.search(r"motor\.yaml:\d+:pole_pairs: 重复键", se) is not None
    expect("duplicate-key", ok, f"rc={rc} se={se!r}")


def test_yaml_syntax(root: Path) -> None:
    cfg = fresh_copy(root)
    append(cfg / "motor.yaml", "bad: [1, 2\n")
    rc, so, se = run(cfg, check=True)
    expect("yaml-syntax-error", rc == 1 and "YAML 解析失败" in se, f"rc={rc} se={se!r}")


def test_pow_guard(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^  vbus_uv_v:.*$", '  vbus_uv_v: "2 ** 1001"')
    rc, so, se = run(cfg, check=True)
    expect("pow-exponent-guard", rc == 1 and "幂指数过大" in se, f"rc={rc} se={se!r}")


def test_bare_ambiguity(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^    i_q_max_a:.*$", '    i_q_max_a: "kp"')
    rc, so, se = run(cfg, check=True)
    expect("bare-reference-ambiguity", rc == 1 and "有歧义" in se, f"rc={rc} se={se!r}")


def test_expression_hex(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "software.yaml", r"^  tx_report_id:.*$", '  tx_report_id: "0x180 + 1"')
    rc, so, se = run(cfg, check=True)
    expect("expression-hex-literal", rc == 0 and "校验通过" in so, f"rc={rc} se={se!r}")


def test_vernier_consistency(root: Path) -> None:
    cfg = fresh_copy(root)
    sub1(cfg / "motor.yaml", r"^reducer_ratio:.*$", "reducer_ratio: 20.0")
    rc, so, se = run(cfg, check=True)
    expect(
        "vernier-consistency",
        rc == 1 and "游标参数不一致" in se,
        f"rc={rc} se={se!r}",
    )


def test_meta_count(root: Path) -> None:
    cfg = fresh_copy(root)
    rc, so, se = run(cfg, check=False)
    n = len(parse_meta(cfg))
    expect("meta-count", rc == 0 and n == len(schema_items()), f"rc={rc} n={n} se={se!r}")


def test_meta_name_sequence(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    names = [m["name"] for m in parse_meta(cfg)]
    expected = [f"{d}.{path}" for (d, path, _cf, _ct) in schema_items()]
    expect("meta-name-sequence", names == expected, f"{names} != {expected}")


def test_meta_apply(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    seen_reboot = set()
    seen_foc_off = set()
    bad = ""
    for m in parse_meta(cfg):
        key = m["name"]
        if key in gen.APPLY_FOC_OFF:
            want = gen.META_APPLY_FOC_OFF
        elif key in gen.APPLY_REBOOT:
            want = gen.META_APPLY_REBOOT
        else:
            want = gen.META_APPLY_LIVE
        if m["apply"] != want:
            bad = f"{key}: {m['apply']} != {want}"
            break
        if m["apply"] == gen.META_APPLY_REBOOT:
            seen_reboot.add(key)
        if m["apply"] == gen.META_APPLY_FOC_OFF:
            seen_foc_off.add(key)
    expect("meta-apply-mapping",
           not bad and seen_reboot == gen.APPLY_REBOOT and seen_foc_off == gen.APPLY_FOC_OFF,
           bad or f"reboot={sorted(seen_reboot)} foc_off={sorted(seen_foc_off)}")


def test_meta_unit(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    unit = {m["name"]: m["unit"] for m in parse_meta(cfg)}
    ok = (unit.get("motor.rs_ohm") == '"Ω"'
          and unit.get("motor.pole_pairs") == "NULL"
          and unit.get("hardware.current_sense.amp_gain") == "NULL")
    expect("meta-unit-extraction", ok,
           f"rs_ohm={unit.get('motor.rs_ohm')} pole_pairs={unit.get('motor.pole_pairs')} "
           f"amp_gain={unit.get('hardware.current_sense.amp_gain')}")


def test_meta_type(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    got = {m["name"]: m["type"] for m in parse_meta(cfg)}
    bad = {f"{d}.{path}": (got.get(f"{d}.{path}"), gen.META_TYPE_ENUMS[ctype])
           for (d, path, _cf, ctype) in schema_items()
           if got.get(f"{d}.{path}") != gen.META_TYPE_ENUMS[ctype]}
    expect("meta-type-mapping", not bad, f"mismatch={bad}")


def test_meta_range(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    meta = {m["name"]: m for m in parse_meta(cfg)}
    duty = meta.get("software.control.limits.duty_max", {})
    kp = meta.get("software.control.current_loop.kp", {})
    ok = (duty.get("min") == "0.501" and duty.get("max") == "1.0"
          and kp.get("min") == "-INFINITY" and kp.get("max") == "INFINITY")
    expect("meta-range-bounds", ok,
           f"duty={duty.get('min')}..{duty.get('max')} kp={kp.get('min')}..{kp.get('max')}")


def test_position_loop_fields(root: Path) -> None:
    """位置环字段：结构体成员 + APPLY_REBOOT 元数据（Task4 管线钉）"""
    cfg = fresh_copy(root)
    rc, _so, _se = run(cfg, check=False)
    src = (cfg / "out" / "params_generated.c").read_text(encoding="utf-8")
    meta = {m["name"]: m for m in parse_meta(cfg)}
    want = {
        "software.control.position_loop.pos_gain": "f32",
        "software.control.position_loop.input_filter_bandwidth": "f32",
    }
    ok = rc == 0 and "position_loop" in src and "pos_gain" in src and "input_filter_bandwidth" in src
    for name, ctype in want.items():
        m = meta.get(name)
        ok = ok and m is not None and m["type"] == gen.META_TYPE_ENUMS[ctype]
        ok = ok and m is not None and m["apply"] == gen.META_APPLY_REBOOT
    expect("position-loop-fields", ok, f"rc={rc} meta={list(meta)}")


def test_meta_deterministic(root: Path) -> None:
    cfg = fresh_copy(root)
    run(cfg, check=False)
    p = cfg / "out" / "params_meta_generated.c"
    first, mtime = p.read_bytes(), p.stat().st_mtime_ns
    run(cfg, check=False)
    second, mtime2 = p.read_bytes(), p.stat().st_mtime_ns
    expect("meta-deterministic", first == second and mtime == mtime2,
           f"same_content={first == second} same_mtime={mtime == mtime2}")


CASES = (
    test_normal,
    test_production_config_check,
    test_motor_kt_flux_consistency,
    test_unknown_key,
    test_missing_required,
    test_undefined_symbol,
    test_cycle,
    test_type_mismatch,
    test_out_of_range,
    test_duplicate_key,
    test_yaml_syntax,
    test_pow_guard,
    test_bare_ambiguity,
    test_expression_hex,
    test_vernier_consistency,
    test_meta_count,
    test_position_loop_fields,
    test_meta_name_sequence,
    test_meta_apply,
    test_meta_unit,
    test_meta_type,
    test_meta_range,
    test_meta_deterministic,
)


def main() -> int:
    root = Path(tempfile.mkdtemp(prefix="gen_params_test_"))
    try:
        for case in CASES:
            try:
                case(root)
            except Exception as e:  # 异常记 FAIL 并继续
                expect(case.__name__, False, f"异常：{e!r}")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    failed = [n for n, ok in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} PASS")
    if failed:
        print("失败用例：" + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
