#!/usr/bin/env bash
# FOC 纯数学层主机自测：宿主机编译 + 运行（无需目标板）
# 用法：scripts/tests/foc/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
CFLAGS="-std=c17 -Wall -Wextra -Werror -O1 -g -I$ROOT/App/Algorithm/FOC/Inc -I$HERE"

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

"$CC" $CFLAGS -o "$OUT/test_foc" "$HERE/test_main.c" "${FILTERED[@]}" "${FOC_SRC[@]}" -lm
"$OUT/test_foc"
