#!/bin/sh
# Build the parity fixture and the shipped worker, then compare CLI, native
# session, Node/WASM worker and Chromium worker final state.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cli=${M33MU_CLI:-"$repo_root/build/m33mu"}
runner=${M33MU_PARITY_RUNNER:-"$repo_root/build/web_parity_runner"}
fixture="$repo_root/tests/firmware/test-stm32h563-web-parity"
"${MAKE:-make}" -C "$fixture" CC="${M33MU_ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}" app.elf >/dev/null
if [ ! -d "$repo_root/web/node_modules/esbuild" ]; then
  "${M33MU_NPM:-npm}" --prefix "$repo_root/web" ci
fi
"$repo_root/scripts/build-web-app.sh" >/dev/null 2>&1
exec "${M33MU_NODE:-node}" "$repo_root/scripts/web-acceptance-matrix.mjs" "$cli" "$runner" \
  "$repo_root/web/public/worker" "$repo_root/web/dist" "$fixture/app.elf" "$@"
