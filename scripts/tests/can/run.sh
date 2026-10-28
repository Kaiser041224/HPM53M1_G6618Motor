#!/usr/bin/env bash
# CAN 协议编解码主机自测（纯函数，无硬件/mock）
# 用法：scripts/tests/can/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=(
    "-I$ROOT/App/Comm/can/Inc"
    "-I$HERE"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

SRC=(
    "$ROOT/App/Comm/can/Src/app_can_protocol.c"
    "$HERE/test_main.c"
    "$HERE/test_can_codec.c"
)

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" "${SRC[@]}" -lm -o "$OUT/can_t1"
"$OUT/can_t1"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" "${SRC[@]}" -lm -o "$OUT/can_t2"
"$OUT/can_t2"

