# Pedigree Restoration Truth

This document is the support contract for this fork. It separates code that is
actively maintained, code that is historically interesting, and behavior that
has actually been verified.

Status checked: 2026-07-29.

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
| x86-64 Linux hosted kernel | Maintained and automated | Built for the native Linux ABI and exercised by the hosted smoke ladder. |
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

One invocation runs these three required lanes:

1. `native-tests`: configure and build the native test support libraries and
   test executable, then run the CTest suite.
2. `asan-tests`: rebuild the utility libraries and tests with AddressSanitizer,
   then run the CTest suite.
3. `hosted-build-and-smoke`: build the x86-64 Linux hosted kernels and their
   required artifacts, then run the hosted smoke ladder and check each
   explicit lifecycle marker.

The populated-initrd rung is intentionally small: it loads the configuration
module and one purpose-built ET_DYN smoke module. It proves the hosted
initrd-loader path without claiming that the complete historical module set is
ready for dynamic hosted loading.

The canonical hosted build sets `PEDIGREE_HOSTED_ASAN=OFF`. AddressSanitizer is
a required part of the native test lane; it is not currently a support claim
for the hosted kernel's signal- and `ucontext`-based scheduler.

The hosted lane always selects the repository's x86-64 Linux Docker image.
Docker, `linux/amd64` container support, Git, CMake/CTest, and a host C/C++
compiler with AddressSanitizer are therefore prerequisites for canonical
`./verify.sh`; verification does not invoke the legacy host package installer.

Each run writes durable output below:

```text
build-verify/logs/<UTC timestamp>/
```

Keep the whole run directory when reporting a failure. A terminal scrollback,
an existing build directory, or one passing test executable is not equivalent
to a green verification run.

## Hosted smoke ladder

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
successfully and every required lane in that invocation passes. In particular:

- all configured native tests passed;
- the separately instrumented ASan build and tests passed without a sanitizer
  report;
- the hosted kernel and required artifacts built successfully;
- every hosted smoke rung reached its required checkpoint and completed in the
  expected way;
- the logs for those results were saved under the same timestamped run
  directory.

Green is deliberately narrow. It does **not** currently prove:

- a clean x86-64 Pedigree GCC/binutils bootstrap on every host;
- creation or boot of the x86-64 PC ISO and disk image;
- an x86-64 PC boot to login in QEMU or on physical hardware;
- broad userspace, POSIX, network, graphics, USB, or device-driver behavior;
- exhaustive filesystem correctness or recovery behavior;
- support for ARM, MIPS, or PowerPC.

When one of those gains an automated, reproducible check, add it as a named
verification lane before expanding the definition of green.

## Hosted development command

For a complete hosted setup and smoke test:

```sh
./easy_build_hosted.sh
```

By default this always selects the repository's x86-64 Linux Docker image.
Non-amd64 hosts require working `linux/amd64` container emulation. The script
bootstraps the required cross-toolchain pieces, builds and tests the native
utilities, builds the hosted kernel, and runs the hosted smoke ladder.

An already provisioned x86-64 Linux host can opt into a direct build:

```sh
PEDIGREE_HOSTED_NATIVE=1 ./easy_build_hosted.sh
```

That direct mode does not install host packages and prints exact incremental
commands using its native build directories. The default container-backed mode
must be rerun through `./easy_build_hosted.sh`; its Linux artifacts and CMake
cache are not directly runnable by macOS or another host ABI.

`./easy_build_hosted.sh` is useful for development, but `./verify.sh` is the
release of record because it also runs the dedicated ASan lane and saves all
lane logs together.

## x86-64 PC status

The active PC configuration is
`build-etc/cmake/pedigree_amd64.cmake`. It describes an x86-64 PC kernel,
Pedigree userspace, GRUB image, and ISO/disk-image targets.

There is not yet a verified end-to-end x86-64 PC command in this restoration
line. `easy_build_x64.sh`, `scripts/qemu`, and the PUP/package instructions are
retained from the historical workflow and must not be treated as current
success criteria. The next acceptable PC workflow needs to:

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
- Hosted AddressSanitizer instrumentation is experimental. Its fiber,
  signal-stack, and context-switch boundaries are not fully annotated, so it is
  excluded from the canonical hosted build; the native ASan lane remains
  required.
- Hosted userspace smoke independently launches `init` and one simple command;
  it does not claim `exec`, `fork`, or `clone`. Hosted scheduler-state
  conversion for `fork`/`clone` and the initial-loader handoff needed by
  `exec` remain unimplemented, so multiprocess and multithreaded userspace are
  not supported by this lane.
- The hosted musl `reboot` translation is sufficient for the root-owned smoke
  shutdown request, but does not implement Linux's full magic/cmd argument
  semantics.
- The complete historical module set is not yet a supported dynamic hosted
  initrd. The canonical populated-initrd rung covers only its purpose-built
  smoke archive; broader dynamic loading currently reaches unverified
  sanitizer, context-switch, and lwIP paths.
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
