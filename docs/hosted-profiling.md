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
cmake -S . -B build-hosted-profile \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build-etc/cmake/pedigree_hosted.cmake" \
  -DPEDIGREE_BUILD_ROLE=TARGET \
  -DPEDIGREE_HOST_TOOLS_MODE=IMPORTED \
  -DIMPORT_EXECUTABLES="$PWD/build-native/tools/HostUtilities.cmake" \
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
  -DPEDIGREE_DEBUG_LOGGING=OFF \
  -DPEDIGREE_MEMORY_LOG=OFF \
  -DPEDIGREE_TRACING=OFF \
  -DPEDIGREE_MODULE_STRIP=/usr/bin/true
cmake --build build-hosted-profile --target kernel configdb -j8
```

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

Each of three repetitions performs one million `getuid` calls, 1,000 seeks,
1,000 seek/writev pairs, and 1,000 seek/readv pairs. Vector operations transfer
4 KiB through two iovecs. Mappings and file backing are prepared before timing;
return values, errno, and payload contents are checked. UID 123 verifies that
the real getuid handler executes. Phase durations use a host monotonic clock.

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
