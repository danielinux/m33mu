#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-wasm-execution-state.XXXXXX")
task_emcmake=${M33MU_EMCMAKE:-emcmake}
task_node=${M33MU_NODE:-node}
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM

export EM_CACHE="$task_build_dir/cache"
"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir"
cmake --build "$task_build_dir" --target execution_state_wasm_smoke web_session_wasm_smoke --parallel
"$task_node" "$task_build_dir/execution_state_wasm_smoke.js"
"$task_node" "$task_build_dir/web_session_wasm_smoke.js"
