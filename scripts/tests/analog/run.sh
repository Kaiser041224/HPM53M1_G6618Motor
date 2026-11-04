#!/usr/bin/env bash
# 模拟量层（app_analog_signal）主机自测：宿主机编译 + 运行（无需目标板）
# 用法：scripts/tests/analog/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=(
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/Interface"
    "-I$HERE"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

shopt -s nullglob
APP_SRC=(
    "$ROOT/App/Platform/Src/app_analog_signal.c"
    "$ROOT/App/Algorithm/Src/algo_filter.c"
)
TEST_SRC=("$HERE"/test_*.c)
MOCK_SRC=("$HERE"/mock_*.c)

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" \
    -o "$OUT/test_analog" "${TEST_SRC[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_analog"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" \
    -o "$OUT/test_analog_fm" "${TEST_SRC[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_analog_fm"
