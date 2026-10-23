#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

scripts/build-web-worker.sh "$repo_root/web/public/worker"
npm --prefix "$repo_root/web" run build
