#!/usr/bin/env bash

set -Eeuo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")/.." && pwd -P)

usage()
{
    cat >&2 <<EOF
Usage:
  $0 --static-kernel PATH --dynamic-kernel PATH \\
     --dynamic-config-module PATH --dynamic-smoke-module PATH \\
     --config PATH --disk-image PATH \\
     [--require-asan] [--expected-heap slam|system] \\
     [--wait-regressions-only]

PEDIGREE_VERIFY_LOG_DIR selects the directory for durable per-rung logs.
PEDIGREE_HOSTED_RUNG_TIMEOUT sets the GNU timeout duration (default: 60s).
EOF
    exit 2
}

static_kernel=
dynamic_kernel=
dynamic_config_module=
dynamic_smoke_module=
configdb=
disk_image=
require_asan=0
expected_heap=
wait_regressions_only=0
rung_timeout=${PEDIGREE_HOSTED_RUNG_TIMEOUT:-60s}

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
            --require-asan)
                require_asan=1
                shift
                ;;
            --expected-heap)
                [ "$#" -ge 2 ] || usage
                expected_heap=$2
                shift 2
                ;;
            --wait-regressions-only)
                wait_regressions_only=1
                shift
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
case "$expected_heap" in
    ""|slam|system) ;;
    *) usage ;;
esac
static_kernel=$(realpath "$static_kernel")
dynamic_kernel=$(realpath "$dynamic_kernel")
dynamic_config_module=$(realpath "$dynamic_config_module")
dynamic_smoke_module=$(realpath "$dynamic_smoke_module")
dynamic_module_dir=$(dirname "$dynamic_smoke_module")
dynamic_users_module=$(realpath "$dynamic_module_dir/users.o")
dynamic_vfs_module=$(realpath "$dynamic_module_dir/vfs.o")
dynamic_fat_module=$(realpath "$dynamic_module_dir/fat.o")
dynamic_rawfs_module=$(realpath "$dynamic_module_dir/rawfs.o")
dynamic_usb_module=$(realpath "$dynamic_module_dir/usb.o")
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

assert_marker_once()
{
    local log=$1
    local marker=$2
    local count
    count=$(grep -aFo "$marker" "$log" | wc -l | tr -d ' ' || true)
    if [ "$count" -ne 1 ]; then
        cat "$log"
        echo "Expected one checkpoint, found $count: $marker" >&2
        return 1
    fi
}

assert_all_wait_markers_once()
{
    local log=$1

    if ! command -v python3 >/dev/null 2>&1; then
        echo "python3 is required to enumerate hosted regression markers." >&2
        return 1
    fi

    python3 "$script_dir/scripts/list-hosted-wait-markers.py" \
        --check-log "$script_dir/src" "$log"
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

assert_asan_kernel()
{
    local kernel=$1
    local probe=$2
    readelf -d "$kernel" >"$probe"
    if ! grep -Fq "libasan.so" "$probe"; then
        echo "Hosted kernel is not linked with AddressSanitizer: $kernel" >&2
        return 1
    fi
}

assert_asan_module()
{
    local module=$1
    local probe=$2
    readelf -Ws "$module" >"$probe"
    if ! grep -Fq "__asan_init" "$probe"; then
        echo "Hosted module is not instrumented by AddressSanitizer: $module" >&2
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
        "SUMMARY: AddressSanitizer" \
        "ERROR: LeakSanitizer" \
        "Page Fault Exception" \
        "KERNELELF: Module relocation failed" \
        "KERNELELF: Hit an invalid module" \
        "invoke attempted with multiple threads" \
        "HOSTED-WAIT-TEST: FAIL" \
        "HOSTED-SYSCALL-TEST: FAIL" \
        "HOSTED-NETWORK-TEST: FAIL" \
        "HOSTED-SMOKE: FAIL" \
        "HOSTED-SMOKE: command exec failed" \
        "HOSTED-SMOKE: command failed" \
        "HOSTED-SMOKE: shutdown request failed"
    do
        reject_marker "$log" "$marker"
    done
}

assert_runtime()
{
    local log=$1
    if [ "$require_asan" = "1" ]; then
        assert_marker "$log" "Hosted runtime: AddressSanitizer;"
    fi
    case "$expected_heap" in
        slam)
            assert_marker "$log" "heap: Pedigree SlamAllocator"
            ;;
        system)
            assert_marker "$log" "heap: system malloc"
            ;;
    esac
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
        env ASAN_OPTIONS="$asan_options" \
            "$timeout_command" --signal=KILL "$rung_timeout" \
                "${args[@]}" >"$log" 2>&1
    ); then
        cat "$log"
        echo "Hosted smoke rung failed to complete: $name" >&2
        return 1
    fi
    assert_clean_log "$log"
    assert_runtime "$log"
}

assert_lifecycle()
{
    local log=$1
    local checkpoint
    for checkpoint in \
        "Pedigree has started: all modules have been loaded." \
        "HOSTED-SHUTDOWN: timers and signals quiesced" \
        "All modules unloaded. Running destructors and terminating..." \
        "trace: kernel main() terminating" \
        "main() returned, cleaning up..."
    do
        assert_marker "$log" "$checkpoint"
    done
}

asan_options="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1:abort_on_error=1:exitcode=99:detect_leaks=0"
if [ "$require_asan" = "1" ]; then
    if ! command -v readelf >/dev/null 2>&1; then
        echo "readelf is required to verify AddressSanitizer artifacts." >&2
        exit 1
    fi
    assert_asan_kernel "$static_kernel" "$scratch_dir/static-kernel.dynamic"
    assert_asan_kernel "$dynamic_kernel" "$scratch_dir/dynamic-kernel.dynamic"
    assert_asan_module \
        "$dynamic_config_module" "$scratch_dir/config-module.symbols"
    assert_asan_module \
        "$dynamic_users_module" "$scratch_dir/users-module.symbols"
    assert_asan_module \
        "$dynamic_vfs_module" "$scratch_dir/vfs-module.symbols"
    assert_asan_module \
        "$dynamic_fat_module" "$scratch_dir/fat-module.symbols"
    assert_asan_module \
        "$dynamic_rawfs_module" "$scratch_dir/rawfs-module.symbols"
    assert_asan_module \
        "$dynamic_usb_module" "$scratch_dir/usb-module.symbols"
    assert_asan_module \
        "$dynamic_smoke_module" "$scratch_dir/smoke-module.symbols"
fi

if [ "$wait_regressions_only" = "0" ]; then
    run_kernel 01-empty-initrd "$static_kernel" "$scratch_dir/empty-initrd.tar"
    empty_log="$log_dir/01-empty-initrd.log"
    assert_marker \
        "$empty_log" "Hosted build has no smoke-test root; shutting down."
    assert_marker_once \
        "$empty_log" \
        "HOSTED-NETWORK-TEST: PASS receive-generation-aba"
    for checkpoint in \
        "HOSTED-SYSCALL-TEST: PASS descriptor-close-pinning" \
        "HOSTED-SYSCALL-TEST: PASS descriptor-close-generation" \
        "HOSTED-SYSCALL-TEST: PASS poll-close-reuse-cleanup" \
        "HOSTED-SYSCALL-TEST: PASS posix-teardown-contention" \
        "HOSTED-SYSCALL-TEST: PASS socket-zero-result-signal" \
        "HOSTED-SYSCALL-TEST: PASS real-event-boundaries"
    do
        assert_marker_once "$empty_log" "$checkpoint"
    done
    assert_lifecycle "$empty_log"
fi

mkdir "$scratch_dir/populated-initrd"
cp "$dynamic_config_module" "$scratch_dir/populated-initrd/config.o"
cp "$dynamic_users_module" "$scratch_dir/populated-initrd/users.o"
cp "$dynamic_vfs_module" "$scratch_dir/populated-initrd/vfs.o"
cp "$dynamic_fat_module" "$scratch_dir/populated-initrd/fat.o"
cp "$dynamic_rawfs_module" "$scratch_dir/populated-initrd/rawfs.o"
cp "$dynamic_usb_module" "$scratch_dir/populated-initrd/usb.o"
cp "$dynamic_smoke_module" "$scratch_dir/populated-initrd/hosted-smoke.o"
(
    cd "$scratch_dir/populated-initrd"
    cmake -E tar cf "$scratch_dir/populated-initrd.tar" \
        --format=gnutar -- \
        config.o users.o vfs.o fat.o rawfs.o usb.o hosted-smoke.o
)

run_kernel \
    02-module-populated-initrd "$dynamic_kernel" \
    "$scratch_dir/populated-initrd.tar"
populated_log="$log_dir/02-module-populated-initrd.log"
assert_marker "$populated_log" "there are 7 files"
assert_marker "$populated_log" "KERNELELF: Preloaded module config"
assert_marker "$populated_log" "KERNELELF: Preloaded module users"
assert_marker "$populated_log" "KERNELELF: Preloaded module vfs"
assert_marker "$populated_log" "KERNELELF: Preloaded module fat"
assert_marker "$populated_log" "KERNELELF: Preloaded module rawfs"
assert_marker "$populated_log" "KERNELELF: Preloaded module usb"
assert_marker "$populated_log" "KERNELELF: Preloaded module hosted-smoke"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: BEGIN"
assert_all_wait_markers_once "$populated_log"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS wake-before-block"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS semaphore-pre-block"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS terminal-cancel-callback-order"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS condition-variable-pre-block"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS condition-variable-completion-barrier"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS condition-variable-contended-signal-reacquire"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS condition-variable-contended-timeout-reacquire"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS condition-variable-terminal-reacquire"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS unlikely-lock-admission"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS hosted-timer-timeout-cleanup"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS mutex-ownership"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS cache-range-existence"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS fat-sector-page-boundary"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS fat-short-read-publication"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS rawfs-native-page-ownership"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS rawfs-parent-alignment-isolation"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS file-past-eof-no-read"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-sync-timeout-cancel-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-sync-rejected-no-callback"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS usb-sync-timeout-completion-handoff"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-pnp-registration-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-ack"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-waitqueue-stop"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-stop-suppresses-republish"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS usb-hcd-port-change-publication"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS timer-clock-deadline"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS timer-handler-lifetime"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS timer-handler-waitqueue-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS pagefault-handler-lifetime"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS pagefault-handler-waitqueue-drain"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS irq-handler-lifetime"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-handler-atomic-drain"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS irq-event-counter-bounded-arithmetic"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-delivery-mode-separation"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-policy-orthogonality"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-writer-lock-self-unregister"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-stale-generation-reuse"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-abandoned-dispatch-cleanup"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS split-irq-coalescing"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS split-irq-lifecycle-serialization"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-threaded-dispatcher-coalescing"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-threaded-hosted-signal"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS pic-threaded-trigger-policy"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS split-irq-unregister-drain"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS split-irq-atomic-shutdown-rejected"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS split-irq-hard-shutdown-rejected"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS split-irq-worker-shutdown-rejected"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS split-irq-shutdown-retry"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS split-irq-hard-callback-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS pic-line-state-mask-lifecycle"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS irq-wait-ready-publication"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS cdi-irq-wait-contract"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS input-callback-lifetime"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS ps2mouse-callback-lifetime"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS log-callback-lifetime"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS log-entry-snapshot"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS network-filter-callback-lifetime"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS cache-callback-lifetime"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS cache-queued-lifetime"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS cache-empty-reuse"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS cache-retirement-publication"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS fresh-thread-timer-progress"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS semaphore-timeout-cancel"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS relay-latest-disposition"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS timeoutguard-cancel-ownership"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS completion-lifecycle"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS terminal-completion-barrier"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS operation-barrier-lifecycle"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS condition-variable-timeout"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS memory-pool-lifecycle"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS memory-pool-close-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS memory-pool-terminal-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS memory-pressure-callback-barrier"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS buffer-close-drain"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS buffer-terminal-drain"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS terminal-operation-admission"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS terminal-timeout-cleanup"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS signal-interruption"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS syscall-handler-lifetime"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS semaphore-signal-after-ordinary-wake"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS condition-signal-after-ordinary-wake"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS ordinary-block-wake"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS process-suspend-resume"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS requestqueue-active-not-backlog"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS requestqueue-worker-terminal-ownership"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS ipc-interruption"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS prequeued-event"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS state-level-publication"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS radix-tree-exported-abi"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS semaphore-drain-available"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS event-delivery-lease"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS event-shutdown-drain"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS thread-join-lifecycle"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS owned-thread-terminal-join"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS lifetime-leases"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS join-terminal-abandonment"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS scheduler-same-priority-progress"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS join-reaper-lease"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS process-exit-rendezvous"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS process-publication-reparent"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS process-exit-election"
assert_marker \
    "$populated_log" "HOSTED-WAIT-TEST: PASS process-resume-vs-termination"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS requestqueue-lifecycle"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS all"
assert_marker "$populated_log" "HOSTED-SMOKE: populated initrd executed"
assert_lifecycle "$populated_log"

if [ "$wait_regressions_only" = "1" ]; then
    echo "Hosted wait regressions passed."
    echo "Logs: $log_dir"
    exit
fi

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
assert_marker \
    "$command_log" "HOSTED-SMOKE: PASS posix-lwip-loopback-roundtrip"
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
