#!/usr/bin/env bash
# 状态 LED 灯语主机自测：宿主机编译 + 运行（无需目标板）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=(
    "-I$ROOT/App/Logic"
    "-I$ROOT/App/Control/Inc"
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/App/Algorithm/FOC/Inc"
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/Interface"
    "-I$HERE"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

shopt -s nullglob
APP_SRC=("$ROOT/App/Logic/app_status_led.c")
TEST_SRC=("$HERE"/test_*.c)
MOCK_SRC=("$HERE"/mock_*.c)

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" \
    -o "$OUT/test_led" "${TEST_SRC[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}"
"$OUT/test_led"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" \
    -o "$OUT/test_led_fm" "${TEST_SRC[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}"
"$OUT/test_led_fm"
