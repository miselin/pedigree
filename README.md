# Pedigree

> [!NOTE]
> This is the active development repository. The restoration is receiving
> substantial AI-assisted maintenance to resolve long-standing bugs and
> stabilize the system. For the pre-AI historical snapshot, see
> [`miselin/pedigree-legacy`](https://github.com/miselin/pedigree-legacy).

Pedigree is a research operating system with its own kernel, modules,
filesystems, POSIX layer, userspace, and build tools. This fork is restoring the
last development baseline around a focused, testable x86-64 scope while
preserving the project as useful systems-software history.

The active source targets are:

- x86-64 PC, built with the Pedigree cross-toolchain;
- native host support and unit tests, built directly on macOS or Linux;
- a focused x86-64 hosted-kernel lifecycle on Apple silicon macOS, run through
  Rosetta without a virtual machine or container.

The native test path is canonical on every supported host. On macOS,
verification also builds an x86-64 Mach-O hosted kernel, loads a focused
Pedigree ELF module, runs the core wait/timer/lifetime/page-fault suite, and
checks clean shutdown. The x86-64 Linux hosted sources remain available for
focused experiments, but their old container-backed runtime matrix is not part
of the maintained public entrypoints. The x86-64 PC kernel, ISO, full
userspace, and QEMU boot path remain active restoration work; they are not
implied by a green host build. ARM, MIPS, and PowerPC material is historical,
not supported.

See [RESTORATION.md](RESTORATION.md) for the exact support boundary, build
commands, verification contract, and known gaps.

## Verify the fork

Run the complete maintained verification set from the repository root:

```sh
./verify.sh
```

The host needs Git, CMake/CTest, and a C/C++ compiler with AddressSanitizer.
Docker is not used. Apple silicon macOS and Linux are both valid hosts. The
macOS hosted-kernel lane additionally needs Rosetta, NASM, and the existing
`compilers/dir` x86-64 Pedigree cross-toolchain.

To bootstrap the retained x86-64 cross-toolchain from its pinned source and
patch set, run:

```sh
python3 scripts/bootstrap_toolchain.py x86_64-pedigree ./pedigree-compiler
```

The command verifies downloaded archives, preserves the `compilers/dir` layout,
and leaves the libc/sysroot integration point at `build/musl` by default.

It builds the native support surface and runs its tests normally and under
AddressSanitizer. On macOS it also runs the focused hosted-kernel lifecycle.
Logs are kept under:

```text
build-verify/logs/<UTC timestamp>/
```

A passing run means those recorded lanes passed for that checkout. It does not
claim an x86-64 PC boot, hardware support, or complete userspace coverage.

## Run native hosted validation

```sh
./easy_build_hosted.sh
```

The historical name is preserved as a public entrypoint. On macOS and Linux it
builds the native kernel-support and selected module-support libraries, the
test suite, and the image/debug utilities, then repeats the test build with
AddressSanitizer. On macOS it also builds and runs the focused x86-64 hosted
kernel lifecycle through Rosetta. It does not build or run the legacy x86-64
Linux hosted kernel.

The retained Linux hosted processor, machine, module-smoke, and Docker files are
non-canonical. They can inform future work, but a green run makes no claim
about that runtime, hosted userspace, full service modules, or its old six-rung
lifecycle ladder.

## Repository map

- `src/system/kernel`: kernel, processor, and machine support
- `src/modules`: loadable and statically linked kernel modules
- `src/modules/subsys`: native and POSIX subsystems
- `src/user`: Pedigree userspace
- `src/buildutil`: native test and image-building utilities
- `images`: filesystem and boot-image inputs
- `scripts`: build, image, and runtime tooling
- `docs`: design notes and historical documentation

## Contributing

Keep changes focused and make the relevant verification lane green. When a
change touches the support boundary or the meaning of green, update
[RESTORATION.md](RESTORATION.md) in the same change.
