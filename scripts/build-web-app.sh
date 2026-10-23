#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_npm=${M33MU_NPM:-npm}
cd "$repo_root"

if [ "${1:-}" = "--install" ]; then
    "$task_npm" --prefix "$repo_root/web" ci
fi

scripts/build-web-worker.sh "$repo_root/web/public/worker"
"$task_npm" --prefix "$repo_root/web" run build
