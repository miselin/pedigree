#!/usr/bin/env bash

# Legacy x86-64 Linux hosted runtime harness. It is retained for focused
# experiments but is not called by easy_build_hosted.sh or verify.sh.

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
     [--wait-regressions-only] [--static-syscall-regressions-only]

  $0 --static-kernel PATH --config PATH \\
     [--require-asan] [--expected-heap slam|system] \\
     --static-syscall-regressions-only

PEDIGREE_VERIFY_LOG_DIR selects the directory for durable per-rung logs.
PEDIGREE_HOSTED_RUNG_TIMEOUT sets the rung deadline in seconds (default: 120).
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
static_syscall_regressions_only=0
rung_timeout=${PEDIGREE_HOSTED_RUNG_TIMEOUT:-120}

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
            --static-syscall-regressions-only)
                static_syscall_regressions_only=1
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

if [ "$wait_regressions_only" = "1" ] &&
    [ "$static_syscall_regressions_only" = "1" ]; then
    echo "Choose either --wait-regressions-only or --static-syscall-regressions-only." >&2
    exit 2
fi
if [[ ! "$rung_timeout" =~ ^[1-9][0-9]*$ ]]; then
    echo "PEDIGREE_HOSTED_RUNG_TIMEOUT must be a positive integer number of seconds." >&2
    exit 2
fi

[ -n "$static_kernel" ] && [ -n "$configdb" ] || usage
if [ "$static_syscall_regressions_only" = "0" ]; then
    [ -n "$dynamic_kernel" ] && [ -n "$dynamic_config_module" ] &&
        [ -n "$dynamic_smoke_module" ] && [ -n "$disk_image" ] || usage
fi
case "$expected_heap" in
    ""|slam|system) ;;
    *) usage ;;
esac
static_kernel=$(realpath "$static_kernel")
configdb=$(realpath "$configdb")
if [ "$static_syscall_regressions_only" = "0" ]; then
    dynamic_kernel=$(realpath "$dynamic_kernel")
    dynamic_config_module=$(realpath "$dynamic_config_module")
    dynamic_smoke_module=$(realpath "$dynamic_smoke_module")
    dynamic_module_dir=$(dirname "$dynamic_smoke_module")
    dynamic_users_module=$(realpath "$dynamic_module_dir/users.o")
    dynamic_vfs_module=$(realpath "$dynamic_module_dir/vfs.o")
    dynamic_fat_module=$(realpath "$dynamic_module_dir/fat.o")
    dynamic_rawfs_module=$(realpath "$dynamic_module_dir/rawfs.o")
    dynamic_usb_module=$(realpath "$dynamic_module_dir/usb.o")
    dynamic_scsi_module=$(realpath "$dynamic_module_dir/scsi.o")
    dynamic_usb_mass_storage_module=$(
        realpath "$dynamic_module_dir/usb-mass-storage.o")
    disk_image=$(realpath "$disk_image")
fi

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

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to enforce hosted rung deadlines." >&2
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

assert_terminal_quiesce_order()
{
    local log=$1
    local admission_marker=$2
    local initial_process_marker=$3
    local handler_marker=$4
    local final_process_marker=$5
    local complete_marker=$6
    if ! awk \
        -v admission_marker="$admission_marker" \
        -v initial_process_marker="$initial_process_marker" \
        -v handler_marker="$handler_marker" \
        -v final_process_marker="$final_process_marker" \
        -v complete_marker="$complete_marker" '
        !reset && index($0, "Resetting...") {
            reset = NR
        }
        reset && !admission && index($0, admission_marker) {
            admission = NR
        }
        reset && !initial_process && index($0, initial_process_marker) {
            initial_process = NR
        }
        reset && !handler && index($0, handler_marker) {
            handler = NR
        }
        reset && !final_process && index($0, final_process_marker) {
            final_process = NR
        }
        reset && !complete && index($0, complete_marker) {
            complete = NR
        }
        reset && !unload && index($0, "KERNELELF: Unloading module") {
            unload = NR
        }
        END {
            exit !(reset && admission && initial_process && handler && final_process &&
                   complete && unload && reset < admission && admission < initial_process &&
                   initial_process < handler && handler < final_process &&
                   final_process < complete && complete < unload)
        }
    ' "$log"; then
        cat "$log"
        echo "POSIX terminal phases did not complete before shutdown module unloading." >&2
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
        "HOSTED-MEMORY-TEST: FAIL" \
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
            python3 "$script_dir/scripts/run-with-deadline.py" \
                --seconds "$rung_timeout" --label "hosted smoke rung $name" -- \
                "${args[@]}" >"$log" 2>&1
    ); then
        cat "$log"
        local last_syscall_phase
        last_syscall_phase=$(grep -aE \
            'HOSTED-SYSCALL-TEST: (BEGIN|PHASE|PASS|FAIL)' "$log" |
            tail -n 1 || true)
        if [ -n "$last_syscall_phase" ]; then
            echo "Last hosted syscall checkpoint: $last_syscall_phase" >&2
        fi
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
        "Module shutdown complete. Running destructors and terminating..." \
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
    if [ "$static_syscall_regressions_only" = "0" ]; then
        assert_asan_kernel \
            "$dynamic_kernel" "$scratch_dir/dynamic-kernel.dynamic"
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
            "$dynamic_scsi_module" "$scratch_dir/scsi-module.symbols"
        assert_asan_module \
            "$dynamic_usb_mass_storage_module" \
            "$scratch_dir/usb-mass-storage-module.symbols"
        assert_asan_module \
            "$dynamic_smoke_module" "$scratch_dir/smoke-module.symbols"
    fi
fi

if [ "$wait_regressions_only" = "0" ] ||
    [ "$static_syscall_regressions_only" = "1" ]; then
    run_kernel 01-empty-initrd "$static_kernel" "$scratch_dir/empty-initrd.tar"
    empty_log="$log_dir/01-empty-initrd.log"
    assert_marker \
        "$empty_log" "Hosted build has no smoke-test root; shutting down."
    assert_marker_once \
        "$empty_log" \
        "HOSTED-NETWORK-TEST: PASS device-lease-deregister-drain"
    assert_marker_once \
        "$empty_log" \
        "HOSTED-NETWORK-TEST: PASS receive-generation-aba"
    for checkpoint in \
        "HOSTED-SYSCALL-TEST: PASS directory-retained-lookup-atomicity" \
        "HOSTED-SYSCALL-TEST: PASS directory-retained-lookup-lifecycle" \
        "HOSTED-SYSCALL-TEST: PASS vfs-established-alias-serialization" \
        "HOSTED-SYSCALL-TEST: PASS descriptor-close-pinning" \
        "HOSTED-SYSCALL-TEST: PASS file-established-alias-lifetime" \
        "HOSTED-SYSCALL-TEST: PASS mmap-established-alias-lifetime" \
        "HOSTED-SYSCALL-TEST: PASS mmap-split-alias-lifetime" \
        "HOSTED-SYSCALL-TEST: PASS posix-path-lookup-lifetime" \
        "HOSTED-SYSCALL-TEST: PASS descriptor-close-generation" \
        "HOSTED-SYSCALL-TEST: PASS poll-close-reuse-cleanup" \
        "HOSTED-SYSCALL-TEST: PASS posix-teardown-contention" \
        "HOSTED-SYSCALL-TEST: PASS socket-zero-result-signal" \
        "HOSTED-SYSCALL-TEST: PASS clone-errno-lifetime" \
        "HOSTED-SYSCALL-TEST: PASS filesystem-unload-policy-metadata" \
        "HOSTED-SYSCALL-TEST: PASS runtime-pinned-cleanup" \
        "HOSTED-SYSCALL-TEST: PASS module-shutdown-retention-policy" \
        "HOSTED-SYSCALL-TEST: PASS real-event-boundaries" \
        "HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-fixture-published" \
        "HOSTED-SYSCALL-TEST: PASS posix-duplicate-init-rollback-preserved-process" \
        "HOSTED-SYSCALL-TEST: PASS posix-terminal-drain-created-fixture-published" \
        "HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-fixture-published" \
        "HOSTED-SYSCALL-TEST: PASS posix-terminal-blocked-handler-released-by-process-exit"
    do
        assert_marker_once "$empty_log" "$checkpoint"
    done
    admission_marker="HOSTED-POSIX-SHUTDOWN: PHASE syscall-admission-closed"
    initial_process_marker="HOSTED-POSIX-SHUTDOWN: PHASE initial-process-drain-complete"
    handler_marker="HOSTED-POSIX-SHUTDOWN: PHASE syscall-handler-drain-complete"
    final_process_marker="HOSTED-POSIX-SHUTDOWN: PHASE final-process-drain-complete"
    terminal_complete_marker="HOSTED-POSIX-SHUTDOWN: PASS terminal-drain synthetic=3 zero-thread=1 threaded=2 zombies=0"
    for checkpoint in \
        "$admission_marker" \
        "$initial_process_marker" \
        "$handler_marker" \
        "$final_process_marker" \
        "$terminal_complete_marker"
    do
        assert_marker_once "$empty_log" "$checkpoint"
    done
    assert_terminal_quiesce_order \
        "$empty_log" \
        "$admission_marker" \
        "$initial_process_marker" \
        "$handler_marker" \
        "$final_process_marker" \
        "$terminal_complete_marker"
    assert_lifecycle "$empty_log"
fi

if [ "$static_syscall_regressions_only" = "1" ]; then
    echo "Hosted static syscall regressions passed."
    echo "Logs: $log_dir"
    exit
fi

mkdir "$scratch_dir/populated-initrd"
cp "$dynamic_config_module" "$scratch_dir/populated-initrd/config.o"
cp "$dynamic_users_module" "$scratch_dir/populated-initrd/users.o"
cp "$dynamic_vfs_module" "$scratch_dir/populated-initrd/vfs.o"
cp "$dynamic_fat_module" "$scratch_dir/populated-initrd/fat.o"
cp "$dynamic_rawfs_module" "$scratch_dir/populated-initrd/rawfs.o"
cp "$dynamic_usb_module" "$scratch_dir/populated-initrd/usb.o"
cp "$dynamic_scsi_module" "$scratch_dir/populated-initrd/scsi.o"
cp "$dynamic_usb_mass_storage_module" \
    "$scratch_dir/populated-initrd/usb-mass-storage.o"
cp "$dynamic_smoke_module" "$scratch_dir/populated-initrd/hosted-smoke.o"
(
    cd "$scratch_dir/populated-initrd"
    cmake -E tar cf "$scratch_dir/populated-initrd.tar" \
        --format=gnutar -- \
        config.o users.o vfs.o fat.o rawfs.o usb.o scsi.o \
        usb-mass-storage.o hosted-smoke.o
)

run_kernel \
    02-module-populated-initrd "$dynamic_kernel" \
    "$scratch_dir/populated-initrd.tar"
populated_log="$log_dir/02-module-populated-initrd.log"
assert_marker "$populated_log" "there are 9 files"
assert_marker "$populated_log" "KERNELELF: Preloaded module config"
assert_marker "$populated_log" "KERNELELF: Preloaded module users"
assert_marker "$populated_log" "KERNELELF: Preloaded module vfs"
assert_marker "$populated_log" "KERNELELF: Preloaded module fat"
assert_marker "$populated_log" "KERNELELF: Preloaded module rawfs"
assert_marker "$populated_log" "KERNELELF: Preloaded module usb"
assert_marker "$populated_log" "KERNELELF: Preloaded module scsi"
assert_marker "$populated_log" "KERNELELF: Preloaded module usb-mass-storage"
assert_marker "$populated_log" "KERNELELF: Preloaded module hosted-smoke"
assert_marker \
    "$populated_log" "HOSTED-MEMORY-TEST: PASS anonymous-region-release"
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
    "$populated_log" "HOSTED-WAIT-TEST: PASS directory-empty-removal-ownership"
assert_marker "$populated_log" "HOSTED-WAIT-TEST: PASS fat-remove-retirement-order"
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
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS scheduler-timer-exit-return-tail"
assert_marker \
    "$populated_log" \
    "HOSTED-WAIT-TEST: PASS hosted-signal-autodisarm-preemption"
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
    "$command_log" "HOSTED-SMOKE: PASS pthread-clone-state-switch"
assert_marker \
    "$command_log" "HOSTED-SMOKE: PASS pthread-child-tls-args-syscall"
assert_marker_once \
    "$command_log" "HOSTED-SMOKE: PASS pthread-clear-tid-join"
assert_marker_once \
    "$command_log" "HOSTED-SMOKE: PASS pthread-clear-tid-detached"
assert_marker \
    "$command_log" "HOSTED-SMOKE: PASS userspace-compute-preemption"
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
