#!/usr/bin/env bash
# OTA 契约主机自测：boot_meta.c（CRC32/镜像头/追加日志/启动确认）+ 镜像头生成器
# 用法：scripts/tests/ota/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)
INCLUDES=("-I$ROOT/Interface")

SRC=(
    "$ROOT/App/Platform/Src/boot_meta.c"
    "$HERE/test_boot_meta.c"
)

echo "── C: boot_meta（-O1）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" "${SRC[@]}" -o "$OUT/boot_meta"
"$OUT/boot_meta"

echo "── C: boot_meta（-O2）"
"$CC" "${BASE_FLAGS[@]}" -O2 "${INCLUDES[@]}" "${SRC[@]}" -o "$OUT/boot_meta_o2"
"$OUT/boot_meta_o2"

echo "── Python: gen_image_header"
python3 "$ROOT/scripts/tests/ota/test_image_header.py"
