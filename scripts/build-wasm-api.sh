#!/bin/sh
# Build the m33mu browser ABI (m33mu_web_api.js + .wasm) into an output
# directory. Used by out-of-tree hosts such as m33mu-lab. Set
# M33MU_WOLFSSL_SRC to a wolfSSL source tree to include the crypto models,
# M33MU_WASM_RUST_SE=ON to link the Rust secure-element simulators.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_output_dir=${1:?usage: build-wasm-api.sh OUTPUT_DIR [BUILD_DIR]}
task_build_dir=${2:-${TMPDIR:-/tmp}/m33mu-wasm-api-build}
task_emcmake=${M33MU_EMCMAKE:-emcmake}

mkdir -p "$task_output_dir" "$task_build_dir"
export EM_CACHE=${EM_CACHE:-$task_build_dir/emscripten-cache}
"$task_emcmake" cmake -S "$repo_root/cmake/wasm" -B "$task_build_dir" \
    ${M33MU_WOLFSSL_SRC:+-DM33MU_WOLFSSL_SRC="$M33MU_WOLFSSL_SRC"} \
    ${M33MU_WASM_RUST_SE:+-DM33MU_WASM_RUST_SE="$M33MU_WASM_RUST_SE"} >/dev/null
cmake --build "$task_build_dir" --target m33mu_web_api --parallel
cp "$task_build_dir/m33mu_web_api.js" "$task_build_dir/m33mu_web_api.wasm" "$task_output_dir/"
