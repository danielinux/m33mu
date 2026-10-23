#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_archive=${1:?usage: scripts/run-ngspice-wasm-callback-smoke.sh ngspice_47+ds.orig.tar.gz [output-dir]}
output_dir=${2:-/tmp/m33mu-ngspice-wasm-callback-smoke}
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-ngspice-wasm.XXXXXX")
task_emconfigure=${M33MU_EMCONFIGURE:-emconfigure}
task_emmake=${M33MU_EMMAKE:-emmake}
task_emcc=${M33MU_EMCC:-emcc}
task_jobs=${M33MU_BUILD_JOBS:-2}
expected_sha256=004b2f7af4c862ed56a89a353e87889b58c21a5a56d13f45a9138d681c3f5dad
trap 'rm -rf "$task_build_dir"' EXIT HUP INT TERM

actual_sha256=$(sha256sum "$source_archive" | cut -d ' ' -f 1)
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "Unexpected ngspice source archive SHA-256: $actual_sha256" >&2
    exit 1
fi

mkdir -p "$task_build_dir/source" "$task_build_dir/build" "$output_dir"
tar -xzf "$source_archive" -C "$task_build_dir/source"
source_dir=$(dirname -- "$(find "$task_build_dir/source" -name configure.ac -print -quit)")
if [[ ! -f "$source_dir/autogen.sh" ]]; then
    echo "Could not locate the ngspice source root in the archive" >&2
    exit 1
fi

(cd "$source_dir" && ./autogen.sh)
python3 - "$source_dir/src/spicelib/analysis/cktsopt.c" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
broken = '''#else
    case OPT_ENH_RSHUNT:
        fprintf(stderr, "WARNING - Option Rshunt available only with XSPICE enabled.\\n");
        break;
#endif'''
if source.count(broken) != 1:
    raise SystemExit("ngspice XSPICE-disabled OPT_ENH_RSHUNT source context changed")
path.write_text(source.replace(broken, "#endif", 1))
PY

export EM_CACHE="$task_build_dir/cache"
(cd "$task_build_dir/build" && \
    "$task_emconfigure" "$source_dir/configure" \
        --with-ngshared \
        --disable-xspice --disable-osdi --disable-klu --disable-openmp \
        --with-readline=no --with-editline=no --disable-maintainer-mode \
        --prefix="$task_build_dir/install")
(cd "$task_build_dir/build" && "$task_emmake" make -j"$task_jobs")

shared_objects=(
    "$task_build_dir/build/src/.libs/libngspice_la-sharedspice.o"
    "$task_build_dir/build/src/.libs/libngspice_la-conf.o"
    "$task_build_dir/build/src/.libs/libngspice_la-ngspice.o"
)
mapfile -t archives < <(find "$task_build_dir/build/src" -path '*/.libs/*.a' -print | sort)
if [[ ${#archives[@]} -lt 50 ]]; then
    echo "Expected ngspice static component archives; found ${#archives[@]}" >&2
    exit 1
fi

"$task_emcc" \
    -I"$source_dir/src/include" -I"$task_build_dir/build/src/include" \
    -fPIC -c "$repo_root/scripts/ngspice_wasm_callback_smoke.c" \
    -o "$task_build_dir/build/callback-smoke.o"
"$task_emcc" \
    "$task_build_dir/build/callback-smoke.o" "${shared_objects[@]}" "${archives[@]}" \
    -o "$output_dir/ngspice-callback-smoke.mjs" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sINVOKE_RUN=1 -sEXIT_RUNTIME=0 \
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152 -sSTACK_OVERFLOW_CHECK=0
(cd "$output_dir" && node --input-type=module -e \
    'import("./ngspice-callback-smoke.mjs").then(async ({default: Module}) => { await Module(); })')
