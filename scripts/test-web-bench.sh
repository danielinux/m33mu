#!/bin/sh
# Build the worker bundle and the analog-loop H563 fixture, then run the
# coupled RC/EXTI bench through the worker under Node.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-web-bench.XXXXXX")
task_emcmake=${M33MU_EMCMAKE:-emcmake}
task_node=${M33MU_NODE:-node}
arm_cc=${M33MU_ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}
fixture_dir="$repo_root/tests/firmware/test-stm32h563-signal-analog-loop"
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM

"${MAKE:-make}" -C "$fixture_dir" CC="$arm_cc" app.elf >/dev/null
objdump=$(dirname "$(command -v "$arm_cc")")/arm-none-eabi-objdump
pass_pc=$("$objdump" -d "$fixture_dir/app.elf" | awk '/bkpt\t0x007f/ { sub(":", "", $1); print $1; exit }')
[ -n "$pass_pc" ] || { echo "pass BKPT not found in fixture" >&2; exit 1; }
export EM_CACHE="$task_build_dir/cache"
"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir" >/dev/null
cmake --build "$task_build_dir" --target m33mu_web_api --parallel >/dev/null
if [ ! -d "$repo_root/web/node_modules/esbuild" ]; then
  "${M33MU_NPM:-npm}" --prefix "$repo_root/web" ci
fi
"$task_node" "$repo_root/scripts/bundle-web-worker.mjs" "$task_build_dir"
"$task_node" "$repo_root/scripts/test-web-bench.mjs" \
  "$task_build_dir/worker.mjs" "$fixture_dir/app.elf" "$pass_pc"
