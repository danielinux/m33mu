#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-web-elf-loader.XXXXXX")
task_emcc=${M33MU_EMCC:-emcc}
task_node=${M33MU_NODE:-node}
trap 'rm -rf "$task_tmp_dir"' EXIT HUP INT TERM

EM_CACHE="$task_tmp_dir/cache" "$task_emcc" \
    -std=c11 -Wall -Wextra -Werror -Wdeclaration-after-statement \
    -sENVIRONMENT=node \
    -I"$repo_root/include" \
    "$repo_root/src/image_loader.c" \
    "$repo_root/tests/image_loader_test.c" \
    -o "$task_tmp_dir/image_loader_test.js"

"$task_node" "$task_tmp_dir/image_loader_test.js"
