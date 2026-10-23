#!/bin/sh
# Build the workbench and run the Chromium interaction tests against web/dist.
# Pass a directory to keep screenshots.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ ! -d "$repo_root/web/node_modules/playwright-core" ]; then
  "${M33MU_NPM:-npm}" --prefix "$repo_root/web" ci
fi
"$repo_root/scripts/build-web-app.sh" >/dev/null
exec "${M33MU_NODE:-node}" "$repo_root/web/tests/browser/workbench.browser.mjs" "$@"
