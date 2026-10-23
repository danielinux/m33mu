#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build_dir=${1:-${TMPDIR:-/tmp}/m33mu-wasm-core-build}
task_emcmake=${M33MU_EMCMAKE:-emcmake}
mkdir -p "$task_build_dir"
export EM_CACHE=${EM_CACHE:-$task_build_dir/emscripten-cache}
mkdir -p "$EM_CACHE"

"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir"
cmake --build "$task_build_dir" --parallel
