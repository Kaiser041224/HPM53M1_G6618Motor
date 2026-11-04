#!/usr/bin/env bash
# 参数持久化宿主自测：生成器产物 + app_param_record + 参数域 store/load
# 用法：scripts/tests/params/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-cc}"
python3 "$ROOT/scripts/gen_params.py" --config-dir "$ROOT/scripts/tests/fixtures" --out-dir "$OUT/gen" >/dev/null

INCLUDES=(
    "-I$ROOT/App/Platform/Inc"
    "-I$ROOT/App/Control/Inc"
    "-I$ROOT/App/Algorithm/FOC/Inc"
    "-I$ROOT/App/Algorithm/Inc"
    "-I$ROOT/App/Comm/terminal/Inc"
    "-I$ROOT/App/Comm/terminal/Src"
    "-I$ROOT/config"
    "-I${HPMDEV_SDK_DIR:-${HPM_SDK_BASE:-/workspace/sdk/hpm_sdk}}/middleware/cherrysh"
    "-I$OUT/gen"
)
BASE_FLAGS=(-std=c17 -Wall -Wextra -Werror)
SRC=(
    "$HERE/test_main.c"
    "$HERE/test_param_terminal_guard.c"
    "$HERE/mock_flash.c"
    "$ROOT/App/Platform/Src/app_param_record.c"
    "$ROOT/App/Platform/Src/app_software_params.c"
    "$ROOT/App/Platform/Src/app_hardware_params.c"
    "$ROOT/App/Control/Src/app_motor_params.c"
    "$OUT/gen/params_generated.c"
    "$OUT/gen/params_meta_generated.c"
)

echo "── pass 1: -O1（常规）"
"$CC" "${BASE_FLAGS[@]}" -O1 -g "${INCLUDES[@]}" -o "$OUT/test_params" "${SRC[@]}"
"$OUT/test_params"

echo "── pass 2: -O2 -ffast-math"
"$CC" "${BASE_FLAGS[@]}" -O2 -ffast-math "${INCLUDES[@]}" -o "$OUT/test_params_fm" "${SRC[@]}"
"$OUT/test_params_fm"
