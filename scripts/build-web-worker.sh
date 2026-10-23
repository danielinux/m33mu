#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_output_dir=${1:-"$repo_root/web/dist/worker"}
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-web-worker-build.XXXXXX")
task_emcmake=${M33MU_EMCMAKE:-emcmake}
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM

mkdir -p "$task_output_dir"
export EM_CACHE="$task_build_dir/cache"
"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir"
cmake --build "$task_build_dir" --target m33mu_web_api --parallel
cp "$task_build_dir/m33mu_web_api.js" "$task_output_dir/"
cp "$task_build_dir/m33mu_web_api.wasm" "$task_output_dir/"
rm -f "$task_output_dir/engine.mjs"
"${M33MU_NODE:-node}" "$repo_root/scripts/bundle-web-worker.mjs" "$task_output_dir"
