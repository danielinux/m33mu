#!/bin/sh
# Run the web TypeScript unit tests (solver, scheduler, project model).
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ ! -d "$repo_root/web/node_modules/esbuild" ]; then
  "${M33MU_NPM:-npm}" --prefix "$repo_root/web" ci
fi
exec "${M33MU_NODE:-node}" "$repo_root/web/tests/run-unit.mjs" "$@"
