# Building Pedigree on Pedigree

The self-host build profile is an experimental first step toward working on a
Pedigree checkout from within Pedigree. It builds the amd64 kernel, dynamic
modules and initrd, configuration database, and in-tree user
applications and libraries. It deliberately does not build an HDD image or
ISO, and it never installs files into `/boot`. Static-driver builds and
compiled distribution keymaps are also excluded from this initial profile.

Cross and native builds consume the same amd64 target profile, so kernel and
userspace ABI settings do not depend on where the compiler is running.

This profile has not yet been verified on a running Pedigree system. The CMake
graph can be exercised from existing cross-build environments, but a successful
native build remains the acceptance test.

## Bootstrap boundary

The first native build still starts with tools and packages installed by an
external seed. It does not rebuild or install its own CMake, compiler, binutils,
NASM, shell, or package dependencies. The selected native GCC must report
`x86_64-pedigree` from `gcc -dumpmachine`; an ordinary Linux compiler is not a
substitute.

That seed must be modern enough for the current source: the maintained
toolchain is GCC 15.3.0, binutils 2.46.1, and NASM 3.02, and the root build
requires C/C++23 plus both `-ftrivial-auto-var-init` modes. The GCC 8.3 files
in the historical `images/local` snapshot are not a usable self-host seed.

Once those tools are available, this slice can rebuild:

- the UEFI-bootable Pedigree kernel;
- kernel modules and the deterministic initrd containing them;
- the configuration database;
- the user applications and libraries defined in this checkout.

That is enough to shorten the edit-build-test loop on Pedigree, while compiler
and package self-bootstrap remain later milestones.

## Prerequisites

- A populated Pedigree source checkout, including required submodules.
- CMake 3.21 or newer.
- A Pedigree-hosted GCC/G++ toolchain targeting `x86_64-pedigree`, plus the
  matching `ar`, `gcc-ar`, `gcc-ranlib`, `ld`, `nm`, `objcopy`, `objdump`,
  `ranlib`, `readelf`, and `strip` tools. The selected toolchain must also
  provide its matching `libgcc` and `libstdc++` runtimes.
- NASM, a POSIX shell, GNU Make, and standard POSIX command-line utilities.
- PUP and its Python runtime, for acquiring the packaged musl SDK. A separate
  `sqlite3`, `tar`, `gzip`, or `patch` command is not required by `boot-artifacts`.
- zlib development headers and library. The native initrd builder links zlib
  directly, so no `gzip` executable is needed.
- Development headers and libraries needed by the in-tree user applications,
  installed under `/usr`. These currently include libpng, Mesa/OSMesa,
  gettext/libintl, dialog, GLib, Pango, Cairo, FreeType, HarfBuzz, Fontconfig,
  and libffi, plus their dependencies.
- Network access for the initial musl PUP download, or a cached copy for an
  offline build. See [musl SDK](musl-sdk.md).

## Libc and syscall boundary

Native amd64 musl uses its upstream Linux register ABI directly. Pedigree's
service-zero syscall entry translates that number inside the POSIX module, so
adding or implementing a Linux-compatible syscall does not require rebuilding
libc unless its public API also changes. Hosted builds retain a separate bridge
because a raw syscall there would enter the host operating system.

The native target does not replace or remove any musl source files and does not
inject Pedigree headers or target macros while compiling it. Its source tree is
the verified upstream archive plus the maintained upstream security backports.
Pedigree-specific headers are provided by the separate platform SDK.

Linux syscall 58 currently uses Pedigree's existing fork implementation. This
preserves the previous safe compatibility behavior while allowing the upstream
musl `vfork` entry point to be used, but does not yet provide Linux's shared-VM,
parent-blocking `vfork` semantics.

The POSIX module owns the amd64 Linux-number table used by this boundary; it no
longer imports musl's private `bits/syscall.h` definitions.

## Cross-build host tools

An ordinary cross build now has one user-facing build tree:

```sh
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build-etc/cmake/pedigree_amd64.cmake \
    -DPEDIGREE_TOOLCHAIN_ROOT=/path/to/pedigree-toolchain
cmake --build build
```

For the same kernel, initrd, and applications boundary used by self-hosting,
disable host-side tests and distribution packaging and build the aggregate
explicitly:

```sh
cmake -S . -B build-boot \
    -DCMAKE_TOOLCHAIN_FILE=build-etc/cmake/pedigree_amd64.cmake \
    -DPEDIGREE_TOOLCHAIN_ROOT=/path/to/pedigree-toolchain \
    -DBUILD_TESTING=OFF \
    -DPEDIGREE_BUILD_HDD_IMAGE=OFF \
    -DPEDIGREE_BUILD_ISO=OFF \
    -DPEDIGREE_BUILD_KEYMAPS=OFF \
    -DPEDIGREE_BUILD_TRANSLATIONS=OFF
cmake --build build-boot --target boot-artifacts
```

CMake acquires the pinned musl SDK through PUP during configuration. Subsequent
builds reuse it without a download or libc compilation.

The target tree owns incremental native sub-builds under `build/host-tools`.
It builds the small configuration-database and initrd generators when they are
needed, and adds the image utilities only when the requested products require
them. Changes to those sources or their CMake files are picked up by the next
`cmake --build build`; there is no sibling tree to refresh or export to import.

There are still separate CMake compiler caches internally. CMake binds one
compiler and platform model to each generated tree, so the target compiler
cannot safely build tools that must run on Linux or macOS. Owning the native
sub-build from the target tree preserves that boundary while giving the normal
cross-build workflow one configure command, one build command, and one target
graph.

The nested build discovers a native `cc`; it only requires a native C++
compiler when image or keymap tools are enabled. Override
`PEDIGREE_BUILD_HOST_C_COMPILER` or `PEDIGREE_BUILD_HOST_CXX_COMPILER` when the
default host compiler is not appropriate. Its caches and executable staging
directories are keyed by that compiler identity, so switching compilers does
not reuse incompatible native outputs.

The self-host profile does not need that split: its compiler produces Pedigree
executables that can run in the same environment, so it builds the generators
directly as target-tree targets.

`PEDIGREE_HOST_TOOLS_MODE=IMPORTED` remains available for specialized build
matrices that deliberately share one native tool build across multiple target
trees. That mode is explicit:

```sh
cmake -S . -B build-host -DPEDIGREE_BUILD_ROLE=HOST_TOOLS
cmake --build build-host --target \
    pedigree-distribution-tools pedigree-configdb pedigree-initrd-builder
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build-etc/cmake/pedigree_amd64.cmake \
    -DPEDIGREE_HOST_TOOLS_MODE=IMPORTED \
    -DIMPORT_EXECUTABLES="$PWD/build-host/HostUtilities.cmake"
```

## First build

With PUP installed, run from the checkout:

```sh
./easy_build_selfhost.sh
```

The default build directory is `build-selfhost`, and the default parallelism is
one job. Useful overrides are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PEDIGREE_BUILD_DIR` | `build-selfhost` | Build directory, relative to the checkout unless absolute |
| `PEDIGREE_BUILD_JOBS` | `1` | Parallel build jobs |
| `PEDIGREE_BUILD_TYPE` | `Debug` | CMake build type |
| `PEDIGREE_NATIVE_TOOL_ROOT` | `/usr` | Prefix containing the native toolchain |
| `PEDIGREE_CMAKE` | `cmake` | CMake executable or absolute path |
| `PEDIGREE_CMAKE_GENERATOR` | CMake default | Optional generator name |

After configuration, repeat just the build with:

```sh
cmake --build build-selfhost --target boot-artifacts --parallel 1
```

Use a new build directory when changing toolchain roots; compiler identities are
cached by CMake.

## Outputs

With the default build directory, the primary products are:

- `build-selfhost/src/system/kernel/kernel` — UEFI boot kernel;
- `build-selfhost/src/modules/initrd.tar` — compressed module initrd;
- `build-selfhost/src/modules/initrd.tar.uncomp` — raw module initrd for the
  UEFI image;
- `build-selfhost/src/modules/initrd.manifest` — deterministic initrd contents;
- `build-selfhost/config.db` — boot configuration database; and
- `build-selfhost/src/user/` — built user applications and libraries;
- `build-selfhost/musl/usr/` — installed libc SDK payload; and
- `build-selfhost/musl/usr/share/pedigree/libc/package.sha256` — installed
  package identity; and
- `build-selfhost/pedigree-c-sdk/usr/` — Pedigree-specific userspace library
  and public headers.

The default UEFI image places the native loader at the removable-media
fallback path and keeps its artifacts in `EFI/PEDIGREE/current` and
`EFI/PEDIGREE/known-good`. To build a GRUB-backed image with those same
variants as chainloadable menu entries, enable `PEDIGREE_BUILD_UEFI_GRUB` and
provide `grub-mkstandalone` (or the target-prefixed equivalent) with
`PEDIGREE_UEFI_GRUB_MKSTANDALONE`.

`boot-artifacts` is an aggregate build target, not an installer or staging
directory. Copying a tested kernel, initrd, and configuration database into a
boot environment is intentionally a separate, manual step for now.

The initrd builder uses zlib at its highest compression level and writes
deterministic gzip metadata. Cross builds compile the utility for their host,
while Pedigree builds compile it directly; neither path needs a `gzip` command.
The configuration database follows the same boundary with the in-tree C
generator. The Python implementations remain regression oracles, not
base-artifact dependencies.

The musl PUP uses installed `/usr` paths and is staged without writing to the
running system. Its loader symlink is relative and remains valid after the
payload is copied into an image. SDK consumers use `usr/include` and `usr/lib`.

Pedigree-specific APIs are staged separately from libc. In particular,
`pedigree_log` is provided by `libpedigree-c` and declared by
`<pedigree/log.h>`, while the framebuffer device ABI is declared by
`<pedigree/fb.h>`. Neither is patched into musl. This keeps the platform API
available to native packages without making it part of the libc provider.

Build dependency installation uses a private SDK root. Updating libc on the
running system is a separate operation; configuring this checkout never does it.

## Building other packages

The Pedigree platform modules currently live in this checkout. Until they are
installed with CMake, native CMake package builds must make them available
explicitly, for example:

```sh
CC=/path/to/native/bin/gcc CXX=/path/to/native/bin/g++ \
    cmake -S package-source -B package-build \
    -DCMAKE_MODULE_PATH=/path/to/pedigree/build-etc/cmake
```

This supplies CMake's Pedigree platform description; each package still owns
its normal dependency and installation requirements. Installing these modules
with the native CMake port is a later bootstrap step.
