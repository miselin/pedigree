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
- x86-64 Linux hosted, built as a Linux process for development and smoke
  testing.

Only the native tests and hosted path are automated today. The x86-64 PC
kernel, ISO, full userspace, and QEMU boot path remain active restoration work;
they are not implied by a green hosted build. ARM, MIPS, and PowerPC material
is historical, not supported.

See [RESTORATION.md](RESTORATION.md) for the exact support boundary, build
commands, verification contract, and known gaps.

## Verify the fork

Run the complete maintained verification set from the repository root:

```sh
./verify.sh
```

The host needs Git, CMake/CTest, a C/C++ compiler with AddressSanitizer, and a
running Docker installation. Docker must be able to run `linux/amd64` images;
Docker Desktop supplies that emulation on Apple silicon.

It runs the native test suite normally and under AddressSanitizer, then runs
the hosted build and smoke ladder. Logs from every lane are kept under:

```text
build-verify/logs/<UTC timestamp>/
```

A passing run means those recorded lanes passed for that checkout. It does not
claim an x86-64 PC boot, hardware support, or complete userspace coverage.

## Build the hosted kernel

```sh
./easy_build_hosted.sh
```

The script always selects its x86-64 Linux Docker image so the maintained path
does not install packages into the host. Non-amd64 hosts need working
`linux/amd64` container emulation. It bootstraps the required toolchain, builds
the native utilities and tests, builds the hosted kernel artifacts, and runs
the six-rung smoke ladder.

On an already provisioned x86-64 Linux machine,
`PEDIGREE_HOSTED_NATIVE=1 ./easy_build_hosted.sh` opts into a direct build. That
mode prints exact incremental rebuild and smoke commands; the container-backed
mode is rerun through `./easy_build_hosted.sh`.

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
