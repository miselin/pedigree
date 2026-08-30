# Pedigree Restoration Truth

This document is the support contract for this fork. It separates code that is
actively maintained, code that is historically interesting, and behavior that
has actually been verified.

Status checked: 2026-08-08.

## Baseline and restoration line

The preserved development baseline is commit
`6c168a1d1288751e065f93f5bdf35fdcc2a8e9ea` from 2023-05-28
(`Fix bug making Config DB read-only`).

The restoration line begins at commit
`4bf325ba65d1eb1ffe38849af6e93a11a8073b86`
(`Modernize hosted easy build`). Work after that point has concentrated on a
repeatable hosted build, allocator and container correctness, high-confidence
VFS/ext2/userland repairs, and removal of unsupported architecture build paths.

The baseline remains available in Git. Historical files that remain in the
tree are reference material, not an assertion that their target still builds.

## Support boundary

| Area | Status | Current claim |
| --- | --- | --- |
| Native test support libraries and unit tests | Maintained and automated | Built and tested on the development host by `./verify.sh`. |
| Native AddressSanitizer lane | Maintained and automated | The test support libraries and unit tests are built with ASan and must pass without sanitizer findings. |
| x86-64 Darwin hosted core | Maintained on Apple silicon macOS | Builds a Mach-O kernel and focused Pedigree ELF module, executes the core wait/timer/lifetime/page-fault suite through Rosetta, and requires clean unload and return to the host. |
| x86-64 Linux hosted kernel | Experimental, non-canonical | The source remains available, but its Docker-backed build and smoke ladder are not required by the maintained entrypoints. |
| x86-64 PC kernel and userspace | Active restoration target | The CMake target and x86-64 source remain in scope, but the current verification contract does not prove a fresh toolchain bootstrap, ISO, userspace image, or QEMU boot. |
| ARM, MIPS, and PowerPC | Historical | Build and boot support was removed from the active fork. Any remaining source or documentation is museum material. |
| Old SCons, Buildbot, Travis, PUP, CDI, and Freenode workflows | Historical | They describe the upstream project at earlier points in its life and are not current build or support instructions. |

“Supported” here means the target is intentionally kept in the active design.
It does not mean every device driver, filesystem, syscall, application, or
hardware configuration has been exercised.

The `src/modules/system/network-stack` module is active integration code. It
connects Pedigree's kernel/network-device interfaces to lwIP; lwIP does not
supersede that layer.

## Canonical verification

From the repository root, run:

```sh
./verify.sh
```

One invocation runs `host-build-and-test`: build the native kernel-support and
selected module-support libraries, test suite, and host utilities, run CTest,
then repeat the test build and run with AddressSanitizer. On macOS the same
stage also builds an x86-64 hosted kernel and focused dynamic module, executes
them through Rosetta, and requires the bounded core regression suite and clean
shutdown markers.

Git, CMake 3.21 or newer, CTest, and a native C23/C++23 compiler with
AddressSanitizer and `-ftrivial-auto-var-init=pattern`/`zero` are the canonical
prerequisites. Docker is not used, and verification does not update submodules
or invoke the legacy host package installer. The macOS hosted lane also
requires Rosetta, NASM, and the GCC 15.3 x86-64 Pedigree cross-toolchain, either
activated as `compilers/dir` or selected with `PEDIGREE_TOOLCHAIN_ROOT`.

Each run writes durable output below:

```text
build-verify/logs/<UTC timestamp>/
```

An opt-in sibling stage replays the x86-64 compile database with GCC 15's
static analyzer and publishes merged SARIF without modifying the ordinary build
objects:

```sh
PEDIGREE_TOOLCHAIN_ROOT=/path/to/pedigree-compiler-15.3.0 \
PEDIGREE_VERIFY_SARIF=1 ./verify.sh
```

Its report is saved as `sarif/pedigree.sarif` inside the same run directory.
This analysis is additional evidence; it is not required by the default green
contract. Analyzer findings are retained for triage and do not themselves fail
the stage.

Keep the whole run directory when reporting a failure. A terminal scrollback,
an existing build directory, or one passing test executable is not equivalent
to a green verification run.

## Retained Linux hosted smoke ladder

The Linux hosted sources and their old smoke harness are retained as
non-canonical development material. The maintained entrypoints do not build or
run this ladder, and it is not part of the definition of green.

The hosted stage advances through six separately logged checkpoints:

| Rung | Required evidence | Saved log |
| --- | --- | --- |
| Empty initrd | A static hosted kernel starts without a root disk and returns through the normal unload lifecycle. | `01-empty-initrd.log` |
| Populated initrd | A minimal dynamic archive preloads and executes `config` and the purpose-built `hosted-smoke` module. | `02-module-populated-initrd.log` |
| Root mount | The hosted disk-image device exposes the generated ext2 image and `mountroot` selects it as `root`. | `03-root-mount.log` |
| Launch init | `/applications/init` is found on that root and entered as the first userspace program. | `04-launch-init.log` |
| Userspace command | `/applications/hosted-smoke-command` is entered independently as the first userspace program and reaches its explicit marker. | `05-userspace-command.log` |
| Clean shutdown | The command requests shutdown and the kernel unloads modules, runs destructors, and returns to its host `main`. | `06-clean-shutdown.log` |

The root, init, command, and shutdown rungs deliberately boot from the same
artifacts in fresh hosted processes with separate physical-memory backing
files. Each process stops at its named checkpoint; the test also rejects a
later checkpoint marker where that would reveal an accidental fall-through.
This makes each checkpoint independently reproducible and prevents one
end-to-end boot from standing in for the whole ladder.

## What green means

A checkout is green only when one `./verify.sh` invocation finishes
successfully and every required stage in that invocation passes. In particular:

- all configured native tests passed;
- the separately instrumented ASan build and tests passed without a sanitizer
  report;
- on macOS, the x86-64 hosted core module completed its bounded regression set
  and the kernel unloaded it and returned to its host `main`;
- the logs for those results were saved under the same timestamped run directory.

Green is deliberately narrow. It does **not** currently prove:

- a clean x86-64 Pedigree GCC/binutils bootstrap on every host;
- creation or boot of the x86-64 PC ISO and disk image;
- an x86-64 PC boot to login in QEMU or on physical hardware;
- an x86-64 Linux hosted-kernel build or lifecycle run;
- hosted userspace, the complete service/module set, or the retired six-rung
  Linux hosted smoke ladder;
- broad userspace, POSIX, network, graphics, USB, or device-driver behavior;
- exhaustive filesystem correctness or recovery behavior;
- support for ARM, MIPS, or PowerPC.

When one of those gains an automated, reproducible check, add it as a named
verification lane before expanding the definition of green.

## Native hosted development command

For a direct native build and test run:

```sh
./easy_build_hosted.sh
```

The historical script name is preserved, but the command is now Docker-free
and runs directly on macOS or Linux. It builds `testsuite`, `headerify`,
`ext2img`, `keymap`, and `memorytracer`, then runs isolated 1 KiB, 4 KiB, and
16 KiB target-page test lanes plus 4 KiB and 16 KiB ASan lanes. On macOS it
also uses the selected GCC 15.3 Pedigree cross-toolchain to build focused 4 KiB
and 16 KiB ELF smoke modules, links the hosted kernels as low-address x86-64
Mach-O executables, and runs both through Rosetta.

The Darwin lifecycle intentionally avoids the historical root filesystem,
full module set, services, musl, and userspace. Its module covers the core
wait, request-queue, lifetime, process-exit, page-fault, timer, primitive, and
signal-interruption regressions. Async scheduler-signal context switching is
not yet part of this bounded lane; the retained Linux scheduler suite depends
on host behavior that does not have Darwin parity yet.

`./easy_build_hosted.sh` is useful for development, but `./verify.sh` is the
release of record because it saves a timestamped result and metadata.

## x86-64 PC status

The active PC configuration is
`build-etc/cmake/pedigree_amd64.cmake`. It describes an x86-64 PC kernel,
Pedigree userspace, GRUB image, and ISO/disk-image targets.

There is not yet a verified end-to-end x86-64 PC command in this restoration
line. `easy_build_x64.sh`, `scripts/qemu`, and the PUP/package instructions are
retained from the historical workflow and must not be treated as current
success criteria. The cross-toolchain bootstrap is now independently available
through `scripts/bootstrap_toolchain.py`; it pins the modern x86-64 Pedigree
toolchain sources and target patches and can build into a side-by-side prefix.
The next acceptable PC workflow needs to:

1. bootstrap or locate the exact cross-toolchain without modifying the source
   checkout unexpectedly;
2. build native host utilities;
3. build the x86-64 kernel, modules, userspace, disk image, and ISO with CMake;
4. boot those artifacts in QEMU from a clean build directory;
5. save the serial log and assert staged boot, userspace, command, and shutdown
   markers.

Until that workflow exists and is in `./verify.sh`, report x86-64 PC results as
focused experiments, not as fork-wide green.

## Current known gaps

- The x86-64 PC QEMU and full userspace path is not a required verification
  lane.
- Hosted execution is a development model, not proof of freestanding machine
  behavior.
- The focused Darwin hosted lane does not exercise async scheduler-signal
  context switching. That remains a parity gap between Darwin and the retained
  Linux hosted runtime.
- Hosted AddressSanitizer instrumentation is experimental. Its fiber,
  signal-stack, and context-switch boundaries are not fully annotated, so it is
  outside the canonical native validation; the native ASan lane remains required.
- Hosted userspace smoke independently launches `init` and one simple command;
  it does not claim `exec`, `fork`, or `clone`. Hosted scheduler-state
  conversion for `fork`/`clone` and the initial-loader handoff needed by
  `exec` remain unimplemented, so multiprocess and multithreaded userspace are
  not supported by this lane.
- The hosted musl `reboot` translation is sufficient for the root-owned smoke
  shutdown request, but does not implement Linux's full magic/cmd argument
  semantics.
- The complete historical module set is not yet a supported dynamic hosted
  initrd. The retained populated-initrd rung covers only its purpose-built smoke
  archive; broader dynamic loading currently reaches unverified sanitizer,
  context-switch, and lwIP paths.
- Test coverage is strongest around utilities and recently repaired
  correctness paths; large subsystems still have sparse automated coverage.
- External ext2 validation and several old runtime harnesses are not part of
  the canonical run until they can execute reliably from a clean checkout.
- Dormant applications and drivers may compile poorly or depend on unavailable
  services. Their presence alone is not a support promise.
- Old documentation may still be valuable for architecture and design context.
  If it describes a retired workflow, it should say “historical” rather than
  silently presenting that workflow as current.

## Documentation rule

Current instructions use CMake and the scripts named above. Do not add SCons,
old CI/download services, or archived community endpoints to active setup
instructions. Preserve genuinely useful historical material with an explicit
historical label and, where practical, a commit or date.
