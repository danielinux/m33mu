#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for task_program in npm node cmake emcmake emcc; do
    if ! command -v "$task_program" >/dev/null 2>&1; then
        echo "webserver: required command not found: $task_program" >&2
        exit 1
    fi
done

cd "$repo_root"
npm --prefix web ci
scripts/build-web-app.sh
exec npm --prefix web run dev
