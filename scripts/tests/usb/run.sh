#!/usr/bin/env bash
# USB bulk 协议服务主机自测：帧层 / 分发 / 参数 / 控制 / 遥测流
# 用法：scripts/tests/usb/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"

# 用测试夹具生成参数元数据（与固件同一生成器，避免手写元数据漂移）
python3 "$ROOT/scripts/gen_params.py" --config-dir "$ROOT/scripts/tests/fixtures" \
    --out-dir "$OUT/gen" >/dev/null

INCLUDES=(
    "-I$HERE"
    "-I$OUT/gen"
    "-I$ROOT/Gui/protocol"
    "-I$ROOT/App/Comm/usb/Inc"
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/App/Control/Inc"
    "-I$ROOT/App/Algorithm/FOC/Inc"
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/App/Debug/Inc"
    "-I$ROOT/Interface"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)

SRC=(
    "$HERE/test_main.c"
    "$HERE/test_usb_proto.c"
    "$HERE/mock_deps.c"
    "$ROOT/scripts/tests/params/mock_flash.c"
    "$ROOT/App/Comm/usb/Src/app_usb_proto.c"
    "$ROOT/App/Platform/Src/app_param_meta.c"
    "$ROOT/App/Platform/Src/app_param_record.c"
    "$ROOT/App/Platform/Src/app_software_params.c"
    "$ROOT/App/Platform/Src/app_hardware_params.c"
    "$ROOT/App/Control/Src/app_motor_params.c"
    "$OUT/gen/params_generated.c"
    "$OUT/gen/params_meta_generated.c"
)

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" -o "$OUT/test_usb" "${SRC[@]}"
"$OUT/test_usb"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" -o "$OUT/test_usb_fm" "${SRC[@]}"
"$OUT/test_usb_fm"
