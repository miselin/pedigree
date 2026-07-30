# Go on Pedigree

The first port targets `pedigree/amd64` with cgo disabled. It reuses Go's
Linux runtime and standard-library files because Pedigree already exposes an
amd64 Linux syscall personality.

## Build

Go 1.26.5 requires Go 1.24.6 or newer as its bootstrap compiler.

```sh
GOROOT_BOOTSTRAP=/path/to/go \
    ./scripts/build-go-pedigree.sh
```

The script verifies the Go source archive, builds the local toolchain, and
creates `build-go-pedigree/go-canary`.

To include that binary in an amd64 Pedigree image, configure the native build
with:

```sh
cmake -DPEDIGREE_GO_CANARY=/absolute/path/to/go-canary ...
```

The image builder installs it as `/applications/go-canary`.

## Current proof

The local fork has been verified to:

- build a host Go 1.26.5 toolchain;
- advertise `pedigree/amd64` through `go tool dist list`;
- select the Linux amd64 runtime, syscall, ELF, and standard-library files;
- produce a static amd64 ELF whose entry point is
  `_rt0_amd64_pedigree`;
- compile the complete standard library with `CGO_ENABLED=0`;
- select a Pedigree-specific, `poll`-based network poller with a nonblocking
  wakeup pipe; and
- compile the complete current Kubernetes kubelet command at upstream commit
  `6fd1049740274a3adfa5d5be904715a780465bf2`.

Pedigree now has per-thread signal masks and alternate signal stacks, and
stores Linux amd64's complete 32-byte `rt_sigaction` state without changing
the native POSIX ABI. On x64, a synchronous CPU fault in a Linux-ABI process
with a custom disposition now creates Linux-compatible `siginfo`, `ucontext`,
legacy FXSAVE, and restorer state. The matching `rt_sigreturn` path validates
and restores the user registers, signal mask, alternate stack, and FPU image.
This direct path is source- and cross-build-verified; asynchronous signals
still use Pedigree's native event frame.

The socket and polling paths now propagate nonblocking state, map the basic
Linux socket options, preserve asynchronous errors and EOF, and account for
data received before an accepted socket is registered. The AF_UNIX stream path
additionally supports Go's nonblocking listen/connect/accept sequence,
`SO_REUSEADDR`, `SO_TYPE`, `SO_ERROR`, `SO_PEERCRED`, bounded pathname
addresses, persistent EOF, and `EPIPE`. Connected streams use shared
connection storage, and pathname lookup pins the target while close, unlink,
or rebind proceeds.
On x64, `AT_RANDOM`, `getrandom`, `/dev/random`, and `/dev/urandom` use
`RDRAND` with an `RDSEED` fallback. They return `EAGAIN`, or omit the
`AT_RANDOM` entry, instead of exposing deterministic bytes when neither
instruction produces data.

The source-level syscall audit is in
[`docs/go-syscall-audit.md`](go-syscall-audit.md). It verifies that missing
`sched_getaffinity` and `madvise` have deliberate Go fallbacks. Local kernel
patches now provide atomic private futex wait/wake enrollment, monotonic and
realtime clocks through both the syscall and vDSO paths, functional
`PROT_NONE`, socket creation flags, `accept4`, and `pipe2`. These changes are
source- and compile-verified. The hosted Linux buildutility harness also
exercises the AF_UNIX implementation under AddressSanitizer, including queued
data before EOF, pending-connect listener close, unlink/rebind identity, and
concurrent connect-versus-close stress. That test runs Pedigree's socket code,
but uses host threads and memory rather than Pedigree's scheduler.

The hosted kernel is not a valid runtime test: an unmodified Go binary's
`SYSCALL` instructions would enter the host kernel instead of Pedigree.
QEMU was intentionally skipped for this pass. The new runtime-facing paths
are source- and cross-build-verified, not exercised inside Pedigree.

The synchronous fault path does not complete the signal ABI. Process-directed
and timer signals still use Pedigree's event trampoline rather than a Linux
frame, signals 32 through 64 have no native delivery slots, and `tgkill` is
absent. The local Go patch therefore keeps asynchronous preemption disabled.
Signal FPU state is legacy FXSAVE only, so AVX upper state is not preserved.
The futex, clock, poller, socket, synchronous-signal, and hardware entropy paths
still need native stress and integration coverage. There is no software entropy
pool, so CPUs or virtual machines without usable `RDRAND` or `RDSEED` cannot
provide random bytes. `/proc/self/exe` is absent, so `os.Executable` currently
returns an error.

The canary's default path checks allocation, a goroutine, channel scheduling,
and output without creating a timer. Setting `GO_CANARY_TIMER=1` adds a timer
gate through the new poller, and `GO_CANARY_UNIX=1` adds bidirectional
pathname AF_UNIX traffic. The final 2,915,890-byte binary has SHA-256
`d082213610130ec8e18423733f46d349d517c6f944fb31bf4632b74faac44d73`.
All three paths pass when that `pedigree/amd64` ELF is run against a Linux
kernel, proving the local Go target and poller independently of Pedigree's
kernel. They have not been run inside Pedigree.

## Native bring-up order

1. Exercise deliberate synchronous fault recovery through the new Linux amd64
   frame and `rt_sigreturn`, including alternate-stack, nested, and FPU cases.
2. Add Linux frames for asynchronous delivery and `tgkill`; keep asynchronous
   Go preemption disabled until those pass targeted stress.
3. Stress the repaired futex, clock, timer, poller, and socket paths on native
   Pedigree.
4. Exercise entropy availability and health, then gate DNS, TCP, HTTPS, and
   mTLS.
5. Gate files and process lifecycle behavior before attempting to start
   kubelet.

## Kubernetes path

Build Kubernetes binaries with `GOOS=pedigree`, not `GOOS=linux`. The local Go
port deliberately makes Linux build constraints and `_linux.go` filenames
match Pedigree, so the kubelet and `x/sys/unix` select their Linux
implementations. Keeping the real target also selects
`_rt0_amd64_pedigree`, disables unsafe signal-based asynchronous preemption,
and leaves truthful Go build metadata.

The compile probe used:

```sh
GOOS=pedigree GOARCH=amd64 CGO_ENABLED=0 GOFLAGS=-mod=vendor \
    /path/to/patched/go build -o kubelet-pedigree ./cmd/kubelet
```

With the Pedigree `poll` backend included, it produced a 132,309,834-byte
static amd64 ELF with SHA-256
`aec0b3fc3649d20f4616519dd695fba897ac39ac6f70db0c3b83cc4e347d1043`.
The binary contains `_rt0_amd64_pedigree` and `runtime.netpoll`. This proves
that the target, Linux build-file alias, and local poller link across the real
kubelet dependency graph. Its `--version` path also runs when the binary is
given a Linux kernel. It does not prove that kubelet can initialize on
Pedigree.

Kubernetes itself currently models Pod operating systems as Linux or Windows.
A small local kubelet shim should therefore advertise this target as Linux
for the `kubernetes.io/os` labels, `NodeInfo.OperatingSystem`, and Pod OS
admission, while retaining `pedigree/amd64` in Go version and user-agent
metadata. Do not globally turn every exact `runtime.GOOS` check into Linux:
cgroups, sysctls, AppArmor, systemd and similar facilities must remain
explicit Pedigree ports.

The practical progression is:

1. Add an explicit heartbeat-only kubelet path that registers a tainted,
   unschedulable Linux/amd64 node with zero Pod capacity and `Ready=False`,
   updates Node status, and renews its Lease without initializing CRI,
   cAdvisor, cgroups, volumes, garbage collection, or Pod synchronization.
2. Implement a small Pedigree CRI v1 service instead of beginning with
   containerd and runc.
3. Run one trusted, preloaded process as a Pod with a shared host network.
4. Add process, filesystem, resource, and per-Pod network isolation.
5. Add storage and conformance coverage.

Do not connect the node to a real control plane until the hardware entropy
path has native availability and health coverage, and asynchronous Linux
signal delivery is complete. A production-shaped node also needs
namespace or equivalent isolation, cgroups or a native resource manager,
per-process statistics, bind mounts, a veth/netlink/CNI equivalent, routing,
NAT or service-proxy support, and a trustworthy process lifecycle API.

Both `_linux.go` and `_pedigree.go` match this port. A future Pedigree-specific
replacement for a Linux file must also change the Linux constraint to
`linux && !pedigree` to avoid duplicate definitions.
