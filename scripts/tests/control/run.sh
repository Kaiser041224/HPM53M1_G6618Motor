#!/usr/bin/env bash
# 控制层（app_foc_current）主机 mock 集成自测：宿主机编译 + 运行（无需目标板）
# 用法：scripts/tests/control/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
INCLUDES=(
    "-I$ROOT/App/Control/Inc"
    "-I$ROOT/App/Algorithm/FOC/Inc"
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/Interface"
    "-I$HERE"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

shopt -s nullglob
APP_SRC=(
    "$ROOT/App/Control/Src/app_foc_current.c"
    "$ROOT/App/Algorithm/FOC/Src/foc_math.c"
    "$ROOT/App/Algorithm/FOC/Src/foc_current.c"
    "$ROOT/App/Algorithm/FOC/Src/foc_modulation.c"
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
    -o "$OUT/test_ctl" "$HERE/test_main.c" "${FILTERED[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_ctl"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" \
    -o "$OUT/test_ctl_fm" "$HERE/test_main.c" "${FILTERED[@]}" "${MOCK_SRC[@]}" "${APP_SRC[@]}" -lm
"$OUT/test_ctl_fm"
