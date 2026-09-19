#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 || $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo "Usage (Linux amd64): $0 BUILD_DIR OUTPUT_DIR [none|perf|stat|strace] [CPU]" >&2
    exit 2
fi

repo=$(cd "$(dirname "$0")/.." && pwd)
build=$(realpath "$1")
mkdir -p "$2"
output=$(realpath "$2")
mode=${3:-perf}
cpu=${4:-0}
divisor=${PEDIGREE_HOSTED_PROFILE_DIVISOR:-1}
kernel="$build/src/system/kernel/kernel"
[[ -x "$kernel" && -f "$build/config.db" && ! -e "$output/run.log" ]] || {
    echo "Build kernel/configdb first, and choose an unused output directory." >&2
    exit 2
}

# Keep recorded addresses tied to the measured binary after subsequent builds.
cp "$kernel" "$output/kernel"
if [[ -f "$kernel.debug" ]]; then
    cp "$kernel.debug" "$output/kernel.debug"
fi
kernel="$output/kernel"

command=("$kernel")
case "$mode" in
    none) ;;
    perf) command=(perf record -F 999 -e cycles:u --call-graph fp
                   -o "$output/perf.data" -- "$kernel") ;;
    stat) command=(perf stat -e cycles:u,instructions:u,task-clock:u,context-switches,page-faults
                   -o "$output/stat.txt" -- "$kernel") ;;
    strace) command=(strace -f -c -o "$output/strace.txt" -- "$kernel")
            divisor=${PEDIGREE_HOSTED_PROFILE_DIVISOR:-100} ;;
    *) echo "Unknown profiling mode: $mode" >&2; exit 2 ;;
esac

scratch=$(mktemp -d "${TMPDIR:-/tmp}/pedigree-hosted-profile.XXXXXXXX")
trap 'rm -rf "$scratch"' EXIT
tar -cf "$scratch/empty.tar" --files-from /dev/null
git -C "$repo" rev-parse HEAD > "$output/commit.txt"
git -C "$repo" diff > "$output/source.diff"
git -C "$repo" status --short > "$output/status.txt"
cp "$build/CMakeCache.txt" "$output/CMakeCache.txt"
sha256sum "$kernel" > "$output/kernel.sha256"
uname -a > "$output/host.txt"
lscpu >> "$output/host.txt"
printf 'mode=%s cpu=%s divisor=%s\n' "$mode" "$cpu" "$divisor" >> "$output/host.txt"

cd "$scratch"
env PEDIGREE_HOSTED_SYSCALL_PROFILE=1 PEDIGREE_HOSTED_PROFILE_DIVISOR="$divisor" \
    uv run --no-project python "$repo/scripts/run-with-deadline.py" \
    --seconds 180 --label hosted-profile -- \
    /usr/bin/time -v -o "$output/host-time.txt" \
    taskset -c "$cpu" "${command[@]}" "$scratch/empty.tar" "$build/config.db" \
    > "$output/run.log" 2>&1

grep -aFq 'HOSTED-PROFILE: PASS all' "$output/run.log"
grep -aFq 'main() returned' "$output/run.log"
if grep -aEq 'HOSTED-PROFILE: FAIL|AddressSanitizer|PANIC|FATAL' "$output/run.log"; then
    echo "Hosted profile failed; see $output/run.log" >&2
    exit 1
fi
if [[ "$mode" == perf ]]; then
    perf report --stdio --no-children --no-inline -g none --percent-limit 0.5 \
        -i "$output/perf.data" > "$output/report.txt"
fi
grep -aF 'HOSTED-PROFILE:' "$output/run.log"
echo "Artifacts: $output"
