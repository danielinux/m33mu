#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_archive=${1:?usage: scripts/run-ngspice-wasm-callback-smoke.sh ngspice_47+ds.orig.tar.gz [output-dir]}
output_dir=${2:-/tmp/m33mu-ngspice-wasm-callback-smoke}
task_build_dir=$(mktemp -d "${TMPDIR:-/tmp}/m33mu-ngspice-wasm.XXXXXX")
keep_build=${M33MU_KEEP_NGSPICE_BUILD:-0}
task_emconfigure=${M33MU_EMCONFIGURE:-emconfigure}
task_emmake=${M33MU_EMMAKE:-emmake}
task_emcc=${M33MU_EMCC:-emcc}
task_jobs=${M33MU_BUILD_JOBS:-2}
expected_sha256=004b2f7af4c862ed56a89a353e87889b58c21a5a56d13f45a9138d681c3f5dad
task_cleanup() {
    if [[ "$keep_build" == 1 ]]; then
        echo "ngspice source/build retained at $task_build_dir" >&2
    else
        rm -rf "$task_build_dir"
    fi
}
trap task_cleanup EXIT HUP INT TERM

if [[ "$keep_build" != 0 && "$keep_build" != 1 ]]; then
    echo "M33MU_KEEP_NGSPICE_BUILD must be 0 or 1" >&2
    exit 1
fi

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

{
    printf 'source_sha256=%s\n' "$actual_sha256"
    printf 'xspice_disabled_workaround=src/spicelib/analysis/cktsopt.c: removed disabled-XSPICE OPT_ENH_RSHUNT branch\n'
    printf 'shared_object=%s\n' "${shared_objects[@]}"
    printf 'static_archive=%s\n' "${archives[@]}"
} > "$output_dir/ngspice-callback-smoke-inputs.txt"

"$task_emcc" \
    -I"$source_dir/src/include" -I"$task_build_dir/build/src/include" \
    -fPIC -c "$repo_root/scripts/ngspice_wasm_callback_smoke.c" \
    -o "$task_build_dir/build/callback-smoke.o"
"$task_emcc" \
    "$task_build_dir/build/callback-smoke.o" "${shared_objects[@]}" "${archives[@]}" \
    -o "$output_dir/ngspice-callback-smoke.mjs" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sINVOKE_RUN=1 -sEXIT_RUNTIME=0 \
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152 -sSTACK_OVERFLOW_CHECK=0 \
    -Wl,-Map="$output_dir/ngspice-callback-smoke.map"

python3 - "$output_dir/ngspice-callback-smoke.map" "$output_dir/ngspice-callback-smoke-inputs.txt" <<'PY'
import pathlib
import re
import sys

map_path, inputs_path = map(pathlib.Path, sys.argv[1:])
archive_members = {}
for line in map_path.read_text().splitlines():
    match = re.search(r"(/[^ ]+?\.a)\(([^)]+)\)", line)
    if match:
        archive_members.setdefault(match.group(1), set()).add(match.group(2))

with inputs_path.open("a") as report:
    report.write(f"linked_archive_count={len(archive_members)}\n")
    report.write(f"linked_archive_member_count={sum(map(len, archive_members.values()))}\n")
    for archive, members in sorted(archive_members.items()):
        report.write(f"linked_member_archive={archive}:{','.join(sorted(members))}\n")
    for name, needle in {
        "numparam": "/frontend/numparam/",
        "sparse": "/maths/sparse/",
        "klu": "/maths/KLU/",
        "osdi": "/osdi/",
        "xspice": "/xspice/",
    }.items():
        matching = [archive for archive in archive_members if needle in archive]
        count = sum(len(archive_members[archive]) for archive in matching)
        report.write(f"linked_exception_{name}_archive_count={len(matching)}\n")
        report.write(f"linked_exception_{name}_member_count={count}\n")
PY

(cd "$output_dir" && node --input-type=module -e \
    'import("./ngspice-callback-smoke.mjs").then(async ({default: Module}) => { await Module(); })')
