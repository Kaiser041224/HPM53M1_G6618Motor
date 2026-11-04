#!/usr/bin/env bash
# 编码器快照 / 应用层主机自测：宿主机编译 + 运行（无需目标板）
# 用法：scripts/tests/encoder/run.sh
# 依赖：bash >= 4.4、cc（可用 CC 覆盖）
# 两遍：① -O1；② -O2 -ffast-math（验证纯逻辑在 fast-math 下仍正确）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=(
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/Interface"
    "-I$HERE"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

shopt -s nullglob
APP_SRC=(
    "$ROOT/App/Algorithm/Src/algo_encoder_snapshot.c"
    "$ROOT/App/Platform/Src/app_encoder.c"
)
TEST_SRC=("$HERE"/test_*.c)
MOCK_SRC=("$HERE"/mock_*.c)

FILTERED=()
for f in "${TEST_SRC[@]}"; do
    if [[ "$(basename "$f")" != "test_main.c" ]]; then
        FILTERED+=("$f")
    fi
done

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" \
    -o "$OUT/test_enc" "$HERE/test_main.c" "${FILTERED[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_enc"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" \
    -o "$OUT/test_enc_fm" "$HERE/test_main.c" "${FILTERED[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_enc_fm"
