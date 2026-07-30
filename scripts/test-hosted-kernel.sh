#!/usr/bin/env bash

set -Eeuo pipefail

usage()
{
    cat >&2 <<EOF
Usage:
  $0 --static-kernel PATH --dynamic-kernel PATH \\
     --dynamic-config-module PATH --dynamic-smoke-module PATH \\
     --config PATH --disk-image PATH

PEDIGREE_VERIFY_LOG_DIR selects the directory for durable per-rung logs.
EOF
    exit 2
}

static_kernel=
dynamic_kernel=
dynamic_config_module=
dynamic_smoke_module=
configdb=
disk_image=

if [ "$#" -gt 0 ]; then
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --static-kernel)
                [ "$#" -ge 2 ] || usage
                static_kernel=$2
                shift 2
                ;;
            --dynamic-kernel)
                [ "$#" -ge 2 ] || usage
                dynamic_kernel=$2
                shift 2
                ;;
            --dynamic-config-module)
                [ "$#" -ge 2 ] || usage
                dynamic_config_module=$2
                shift 2
                ;;
            --dynamic-smoke-module)
                [ "$#" -ge 2 ] || usage
                dynamic_smoke_module=$2
                shift 2
                ;;
            --config)
                [ "$#" -ge 2 ] || usage
                configdb=$2
                shift 2
                ;;
            --disk-image)
                [ "$#" -ge 2 ] || usage
                disk_image=$2
                shift 2
                ;;
            *)
                usage
                ;;
        esac
    done
else
    usage
fi

[ -n "$static_kernel" ] && [ -n "$dynamic_kernel" ] &&
    [ -n "$dynamic_config_module" ] &&
    [ -n "$dynamic_smoke_module" ] && [ -n "$configdb" ] &&
    [ -n "$disk_image" ] || usage
static_kernel=$(realpath "$static_kernel")
dynamic_kernel=$(realpath "$dynamic_kernel")
dynamic_config_module=$(realpath "$dynamic_config_module")
dynamic_smoke_module=$(realpath "$dynamic_smoke_module")
configdb=$(realpath "$configdb")
disk_image=$(realpath "$disk_image")

scratch_dir=$(mktemp -d)
trap 'rm -rf "$scratch_dir"' EXIT

if [ -n "${PEDIGREE_VERIFY_LOG_DIR:-}" ]; then
    mkdir -p "$PEDIGREE_VERIFY_LOG_DIR"
    log_dir=$(cd -P -- "$PEDIGREE_VERIFY_LOG_DIR" && pwd -P)
else
    run_id=$(date -u +%Y%m%dT%H%M%SZ)
    log_root="$(dirname "$configdb")/smoke-logs"
    log_dir="$log_root/$run_id"
    mkdir -p "$log_root"
    if ! mkdir "$log_dir"; then
        echo "Hosted smoke log directory already exists: $log_dir" >&2
        exit 2
    fi
fi

if ! mkdir "$log_dir/.claimed"; then
    echo "Hosted smoke log directory is already in use: $log_dir" >&2
    exit 2
fi

if command -v timeout >/dev/null 2>&1; then
    timeout_command=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    timeout_command=gtimeout
else
    echo "GNU timeout (or gtimeout) is required." >&2
    exit 1
fi

mkdir "$scratch_dir/empty-initrd"
(
    cd "$scratch_dir/empty-initrd"
    cmake -E tar cf "$scratch_dir/empty-initrd.tar" --format=gnutar -- .
)

assert_marker()
{
    local log=$1
    local marker=$2
    if ! grep -aFq "$marker" "$log"; then
        cat "$log"
        echo "Missed checkpoint: $marker" >&2
        return 1
    fi
}

reject_marker()
{
    local log=$1
    local marker=$2
    if grep -aFq "$marker" "$log"; then
        cat "$log"
        echo "Unexpected failure marker: $marker" >&2
        return 1
    fi
}

assert_clean_log()
{
    local log=$1
    local marker
    for marker in \
        "ERROR: AddressSanitizer" \
        "AddressSanitizer:DEADLYSIGNAL" \
        "Page Fault Exception" \
        "KERNELELF: Module relocation failed" \
        "KERNELELF: Hit an invalid module" \
        "invoke attempted with multiple threads" \
        "HOSTED-SMOKE: command exec failed" \
        "HOSTED-SMOKE: command failed" \
        "HOSTED-SMOKE: shutdown request failed"
    do
        reject_marker "$log" "$marker"
    done
}

run_kernel()
{
    local name=$1
    local kernel=$2
    local initrd=$3
    local disk=${4:-}
    local stop_after=${5:-}
    local log="$log_dir/$name.log"
    local rung_dir="$scratch_dir/$name"
    local args=("$kernel" "$initrd" "$configdb")
    if [ -n "$disk" ]; then
        args+=("$disk")
    fi
    if [ -n "$stop_after" ]; then
        [ -n "$disk" ] || {
            echo "A stop stage requires a disk image." >&2
            return 2
        }
        args+=("$stop_after")
    fi

    mkdir "$rung_dir"
    echo "Running hosted smoke rung: $name"
    if ! (
        cd "$rung_dir"
        "$timeout_command" --signal=KILL 60s "${args[@]}" >"$log" 2>&1
    ); then
        cat "$log"
        echo "Hosted smoke rung failed to complete: $name" >&2
        return 1
    fi
    assert_clean_log "$log"
}

assert_lifecycle()
{
    local log=$1
    local checkpoint
    for checkpoint in \
        "Pedigree has started: all modules have been loaded." \
        "All modules unloaded. Running destructors and terminating..." \
        "trace: kernel main() terminating" \
        "main() returned, cleaning up..."
    do
        assert_marker "$log" "$checkpoint"
    done
}

run_kernel 01-empty-initrd "$static_kernel" "$scratch_dir/empty-initrd.tar"
empty_log="$log_dir/01-empty-initrd.log"
assert_marker "$empty_log" "Hosted build has no smoke-test root; shutting down."
assert_lifecycle "$empty_log"

mkdir "$scratch_dir/populated-initrd"
cp "$dynamic_config_module" "$scratch_dir/populated-initrd/config.o"
cp "$dynamic_smoke_module" "$scratch_dir/populated-initrd/hosted-smoke.o"
(
    cd "$scratch_dir/populated-initrd"
    cmake -E tar cf "$scratch_dir/populated-initrd.tar" \
        --format=gnutar -- config.o hosted-smoke.o
)

run_kernel \
    02-module-populated-initrd "$dynamic_kernel" \
    "$scratch_dir/populated-initrd.tar"
populated_log="$log_dir/02-module-populated-initrd.log"
assert_marker "$populated_log" "there are 2 files"
assert_marker "$populated_log" "KERNELELF: Preloaded module config"
assert_marker "$populated_log" "KERNELELF: Preloaded module hosted-smoke"
assert_marker "$populated_log" "HOSTED-SMOKE: populated initrd executed"
assert_lifecycle "$populated_log"

run_kernel \
    03-root-mount "$static_kernel" "$scratch_dir/empty-initrd.tar" \
    "$disk_image" root
root_log="$log_dir/03-root-mount.log"
assert_marker "$root_log" "successfully as root."
assert_marker "$root_log" "HOSTED-SMOKE: root mounted"
reject_marker "$root_log" "HOSTED-SMOKE: init launched"
assert_lifecycle "$root_log"

run_kernel \
    04-launch-init "$static_kernel" "$scratch_dir/empty-initrd.tar" \
    "$disk_image" init
init_log="$log_dir/04-launch-init.log"
assert_marker "$init_log" "successfully as root."
assert_marker "$init_log" "HOSTED-SMOKE: init launched"
reject_marker "$init_log" "HOSTED-SMOKE: simple userspace command ran"
assert_lifecycle "$init_log"

run_kernel \
    05-userspace-command "$static_kernel" "$scratch_dir/empty-initrd.tar" \
    "$disk_image" command
command_log="$log_dir/05-userspace-command.log"
reject_marker "$command_log" "HOSTED-SMOKE: init launched"
assert_marker "$command_log" "HOSTED-SMOKE: simple userspace command ran"
reject_marker "$command_log" "HOSTED-SMOKE: requesting clean shutdown"
assert_lifecycle "$command_log"

run_kernel \
    06-clean-shutdown "$static_kernel" "$scratch_dir/empty-initrd.tar" \
    "$disk_image" shutdown
shutdown_log="$log_dir/06-clean-shutdown.log"
reject_marker "$shutdown_log" "HOSTED-SMOKE: init launched"
assert_marker "$shutdown_log" "HOSTED-SMOKE: simple userspace command ran"
assert_marker "$shutdown_log" "HOSTED-SMOKE: requesting clean shutdown"
assert_lifecycle "$shutdown_log"

echo "Hosted smoke ladder passed."
echo "Logs: $log_dir"
