#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-wasm-worker.XXXXXX")
task_emcmake=${M33MU_EMCMAKE:-emcmake}
task_node=${M33MU_NODE:-node}
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM

export EM_CACHE="$task_build_dir/cache"
"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir"
cmake --build "$task_build_dir" --target m33mu_web_api --parallel
cp "$repo_root/web/worker/engine.mjs" "$task_build_dir/engine.mjs"
cp "$repo_root/web/worker/worker.mjs" "$task_build_dir/worker.mjs"
"$task_node" "$repo_root/scripts/test-web-worker.mjs" \
  "$task_build_dir/worker.mjs"
