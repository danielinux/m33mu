#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_npm=${M33MU_NPM:-npm}

for task_program in "$task_npm" ${M33MU_NODE:-node} cmake ${M33MU_EMCMAKE:-emcmake}; do
    if ! command -v "$task_program" >/dev/null 2>&1; then
        echo "webserver: required command not found: $task_program" >&2
        exit 1
    fi
done

cd "$repo_root"
scripts/build-web-app.sh --install
exec "$task_npm" --prefix web run dev
