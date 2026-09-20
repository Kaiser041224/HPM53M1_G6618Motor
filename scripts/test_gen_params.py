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


CASES = (
    test_normal,
    test_production_config_check,
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
