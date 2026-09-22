# Native Linux hosted profiling

This lane runs the actual hosted kernel on Linux amd64, with Pedigree's SLAM,
scheduler, locks, POSIX dispatcher, usercopy, VFS, and RamFS. It is separate from
the native support-code tests run by `easy_build_hosted.sh` on Linux.

The initial workload replays Linux-compatible syscalls from a Pedigree kernel
thread. It excludes userspace entry assembly and userspace return gates. The
normal accounting code still executes for these kernel-origin calls. Raw
`syscall` instructions in an ordinary Linux executable enter Linux, not Pedigree.

## Build

First complete the native tools build (`easy_build_hosted.sh`) and install the
Pedigree cross compiler (`easy_build_x64.sh` provides it). Keep this profiling
build separate from both. Install `perf`, `uv`, and optionally `strace`/Valgrind.

```sh
HOST_TOOLS_BUILD="$PWD/build-native/tools"
cmake -S . -B build-hosted-profile \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build-etc/cmake/pedigree_hosted.cmake" \
  -DPEDIGREE_BUILD_ROLE=TARGET \
  -DPEDIGREE_HOST_TOOLS_MODE=IMPORTED \
  -DIMPORT_EXECUTABLES="$HOST_TOOLS_BUILD/HostUtilities.cmake" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPEDIGREE_HOSTED_DYNAMIC_MODULES=OFF \
  -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
  -DPEDIGREE_HOSTED_SYSTEM_MALLOC=OFF \
  -DPEDIGREE_HOSTED_ASAN=OFF \
  -DPEDIGREE_MODULE_usb-hcd=OFF \
  -DPEDIGREE_WITH_INIT=OFF \
  -DPEDIGREE_BUILD_USER_DIR=OFF \
  -DPEDIGREE_BUILD_HDD_IMAGE=OFF \
  -DPEDIGREE_BUILD_ISO=OFF \
  -DPEDIGREE_BUILD_UEFI=OFF \
  -DPEDIGREE_BUILD_KEYMAPS=OFF \
  -DPEDIGREE_BUILD_TRANSLATIONS=OFF \
  -DPEDIGREE_OPTIMIZE=ON \
  -DPEDIGREE_OPTIMIZE_SIZE=ON \
  -DPEDIGREE_WARNINGS=ON \
  -DPEDIGREE_DEBUG_LOGGING=OFF \
  -DPEDIGREE_MEMORY_LOG=OFF \
  -DPEDIGREE_TRACING=OFF \
  -DPEDIGREE_MODULE_STRIP=/usr/bin/true
cmake --build build-hosted-profile --target kernel configdb -j8
```

`HOST_TOOLS_BUILD` must name a completed host-tools build. The path above is
the default from `easy_build_hosted.sh`; an overridden
`PEDIGREE_NATIVE_BUILD_ROOT` uses its `tools` child. CMake exports
`HostUtilities.cmake` at the standalone host-tools build root. Do not point this
import at the staged executables of a nested target build.

The explicit optimization options retain `-Os` despite the debug build type.
Frame pointers are already enabled. Disabling module stripping preserves their
DWARF until the final kernel debug split; keep `kernel.debug` beside `kernel`.
Hosted smoke builds also retain local function names. Keep these settings fixed
across comparisons: the hosted kernel reads its own ELF symbol table at startup.

## Run

Choose one CPU permitted by the host affinity mask. Do not build or run another
benchmark concurrently. Each invocation needs a fresh output directory:

```sh
scripts/profile-hosted.sh build-hosted-profile /tmp/hosted-time none 0
scripts/profile-hosted.sh build-hosted-profile /tmp/hosted-perf perf 0
scripts/profile-hosted.sh build-hosted-profile /tmp/hosted-stat stat 0
scripts/profile-hosted.sh build-hosted-profile /tmp/hosted-strace strace 0
```

The runner sets `PEDIGREE_HOSTED_SYSCALL_PROFILE=1`, creates a temporary physical
memory backing, boots with a minimal RAMfs root, runs the workload, and requires
both the benchmark PASS marker and orderly kernel return. It enforces a deadline
and cleans temporary backing files. It saves the measured kernel and debug file
with the results so later rebuilds cannot invalidate the profile. Normal hosted
smoke behavior is unchanged when the environment setting is absent.

Before timing, the profile runs focused hosted address-space checks: isolation
across switches, global kernel mappings, permission changes, mapping reuse,
clone ownership, copy-on-write, and teardown. These must print
`HOSTED-WAIT-TEST: PASS hosted-vas-mapping-index` before the workload begins.
Post-syscall checks also validate owned state payloads, nested dispatch, rejected
duplicate actions, and dispatch-context restoration before timing begins.
Accounting checks verify thread/process totals and deferred timer reporting,
including timer arming and shutdown, before the measured workload.

Each of three repetitions performs one million `getuid` calls, 1,000 seeks,
1,000 seek/writev pairs, and 1,000 seek/readv pairs. Vector operations transfer
4 KiB through two iovecs. Mappings and file backing are prepared before timing;
return values, errno, and payload contents are checked. UID 123 verifies that
the real getuid handler executes. Phase durations use a host monotonic clock.

Set `PEDIGREE_HOSTED_PROFILE_PHASE=getuid` to run only the three getuid loops,
after 1,000 untimed warmup calls. This skips the I/O fixture's mappings and file
operations, while retaining the startup regression checks and normal process
setup. The default is `all`. The selector works in every profiling mode.

`run.log` contains phase timings; `host-time.txt` contains Linux process wall,
user, and system time including startup and shutdown. These are distinct from
Pedigree's own user/system accounting. `perf.data` contains sampled user-mode CPU
cycles, including Pedigree and host libraries; `report.txt` is a symbolized report.
Host Linux kernel execution is excluded from `cycles:u`. `strace.txt` reveals
host syscalls and their counts; tracing timings are perturbed and must not be
used as benchmark results. The strace mode divides iteration counts by 100 to
bound tracing overhead. Set `PEDIGREE_HOSTED_PROFILE_DIVISOR` explicitly (1 to
100000) to change this scaling in any mode; every phase reports its actual count.

Hosted interrupt masking uses `pthread_sigmask`; address-space operations use
host VM syscalls. Attribute those separately from portable kernel work. A hosted
profile can locate expensive algorithms and calls, but cannot establish the
bare-metal cost of IRQ masking, page-table operations, or syscall entry.

For workload attribution, filter sampled callchains to `hostedProfileGetuid`,
`hostedProfileLseek`, `hostedProfileWritev`, or `hostedProfileReadv`, excluding
`reportPhase` and logging. The whole-process report includes boot, regression
checks, and shutdown. In particular, mapping allocation during ELF symbol loading
is not syscall-loop work. Preserve unresolved sample addresses as unknown rather
than attributing them to a nearby kernel function.

## Unsampled function profiles

Configure a separate build with the options above and
`-DPEDIGREE_HOSTED_FUNCTION_PROFILE=ON`, then build `kernel configdb`. This adds
GCC entry/exit hooks to the kernel and static modules while retaining normal
optimization and inlining. The recorder itself is not instrumented.

```sh
scripts/profile-hosted.sh build-hosted-instrument /tmp/hosted-functions instrument 0
callgrind_annotate --auto=no /tmp/hosted-functions/callgrind.getuid
```

The default capture caps each phase at 100 iterations, across three repetitions.
Set `PEDIGREE_HOSTED_FUNCTION_PROFILE_LIMIT` (1 to 10000) to change the cap;
`PEDIGREE_HOSTED_PROFILE_DIVISOR` still reduces the underlying workload first.
The warmed loops mask hosted IRQs; setup, reporting, and teardown run normally.
Scheduling, C++ context transfers, or IRQ-enable requests invalidate the capture
instead of silently bypassing required work. These kernel-origin RamFS tests
are expected to need none. This mode does not support general concurrent or
userspace-origin workloads, and does not prevent Linux from preempting the host
process. Synchronous exceptions remain deliverable.

Each `<phase>-<repetition>.bin` contains raw little-endian records with four
64-bit fields: TSC timestamp, function address, machine call site, and event kind
(1 entry, 2 exit). The matching JSON records iteration/event counts, elapsed
nanoseconds, dropped events, and invalidation reason (0 valid, 1 scheduling,
2 IRQ enable, 3 context transfer). A pre-faulted 64 MiB buffer holds each capture;
overflow is a failed capture, never silently accepted. The analyzer also rejects
unbalanced events, invalid lengths, and backwards timestamps.

`report.txt` and `callgrind.<phase>` aggregate repetitions into exact recorded
invocation counts and inclusive/exclusive TSC ticks. KCachegrind can open these
files without Valgrind. GCC emits logical inline-function boundaries, so the
analyzer uses event nesting rather than the machine return address to reconstruct
parents. Unknown function addresses remain hexadecimal. Assembly and host library
internals are not instrumented; their time remains charged to the surrounding
instrumented function.

TSC ticks are elapsed instrumented time, not CPU cycles. Hook overhead, host
preemption, and compiler changes affect timings, particularly tiny getters.
Use these profiles to identify repeated work and call paths; verify performance
changes with the ordinary build. An instrumented binary retains callback costs
even when recording is off. Capture timings must not be compared to ordinary
benchmark timings.

To regenerate reports from saved captures:

```sh
uv run --no-project python scripts/analyze-hosted-functions.py /tmp/hosted-functions
```

## Valgrind compatibility

Callgrind is not currently a validated runner mode. On Linux amd64 with
Valgrind 3.27.1, the hosted kernel aborts before the workload with Callgrind's
`vgCallgrind_post_signal` assertion (`sigNum == current_state.sig`). Keeping PLT
frames with `--skip-plt=no` also fails. This matches the signal/alternate-stack
failure described in [Valgrind bug 339160](https://bugs.kde.org/show_bug.cgi?id=339160).
Callgrind's shadow-stack handling needs investigation before its call counts
can be trusted with the hosted signal and context-switch paths.

A Cachegrind attempt with the same kernel reached module startup but faulted
while starting `splash`; it did not reach the workload either. These are tool
compatibility results, not performance measurements. Continue using the native
`perf` mode until a Valgrind run passes the workload and shutdown checks.
