#!/usr/bin/env bash
# FOC 纯数学层主机自测：宿主机编译 + 运行（无需目标板）
# 用法：scripts/tests/foc/run.sh
# 依赖：bash >= 4.4（空数组展开）、cc（可用 CC 覆盖）
# 两遍：① 常规 -O1；② -O2 -ffast-math（验证 foc_finite 位级判断在 fast-math 下仍有效）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=("-I$ROOT/App/Algorithm/FOC/Inc" "-I$HERE")
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

shopt -s nullglob
FOC_SRC=("$ROOT"/App/Algorithm/FOC/Src/*.c)
TEST_SRC=("$HERE"/test_*.c)

# 排除 test_main.c（避免重复编译）
FILTERED=()
for f in "${TEST_SRC[@]}"; do
    if [[ "$(basename "$f")" != "test_main.c" ]]; then
        FILTERED+=("$f")
    fi
done

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" \
    -o "$OUT/test_foc" "$HERE/test_main.c" "${FILTERED[@]}" "${FOC_SRC[@]}" -lm
"$OUT/test_foc"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" \
    -o "$OUT/test_foc_fm" "$HERE/test_main.c" "${FILTERED[@]}" "${FOC_SRC[@]}" -lm
"$OUT/test_foc_fm"
