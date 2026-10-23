#!/bin/sh
# Run the H563 LED blink and RC button demos through the worker bench.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-web-demos.XXXXXX")
task_node=${M33MU_NODE:-node}
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM
examples="$repo_root/web/public/examples"
export EM_CACHE="$task_build_dir/cache"
"${M33MU_EMCMAKE:-emcmake}" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir" >/dev/null
cmake --build "$task_build_dir" --target m33mu_web_api --parallel >/dev/null
if [ ! -d "$repo_root/web/node_modules/esbuild" ]; then
  "${M33MU_NPM:-npm}" --prefix "$repo_root/web" ci
fi
"$task_node" "$repo_root/scripts/bundle-web-worker.mjs" "$task_build_dir"
"$task_node" "$repo_root/scripts/test-web-demos.mjs" "$task_build_dir/worker.mjs" \
  "$examples/h563-blink.elf" "$examples/h563-button.elf" "$repo_root/web/fixtures/demos/golden.json"
