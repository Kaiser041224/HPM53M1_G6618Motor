#!/usr/bin/env bash
# 全量主机自测：布局守卫 + 全部套件 + 参数生成器。
# 用法：scripts/tests/all.sh          （需先 make artifacts，供布局守卫使用）
#       SKIP_LAYOUT=1 scripts/tests/all.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

fail=0
run() {
    local name="$1"; shift
    local log
    log="$(mktemp)"
    if "$@" >"$log" 2>&1; then
        printf '%-10s OK   %s\n' "$name" "$(grep -E 'checks, [0-9]+ failures|PASS|/23' "$log" | tail -1)"
    else
        printf '%-10s FAIL (rc=%s)\n' "$name" "$?"
        tail -20 "$log"
        fail=1
    fi
    rm -f "$log"
}

if [[ "${SKIP_LAYOUT:-0}" != "1" ]]; then
    run layout bash scripts/tests/layout/check_layout.sh
fi
for t in adc analog can control fault foc encoder identify params; do
    run "$t" bash "scripts/tests/$t/run.sh"
done
run gen python3 scripts/test_gen_params.py

if [[ "$fail" -ne 0 ]]; then
    echo "全量自测: FAIL"
    exit 1
fi
echo "全量自测: PASS"