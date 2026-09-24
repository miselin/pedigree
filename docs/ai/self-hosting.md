# Building Pedigree on Pedigree

The self-host build profile is an experimental first step toward working on a
Pedigree checkout from within Pedigree. It builds the amd64 kernel, dynamic
modules and initrd, and in-tree user applications and libraries. It does not
build an HDD image, ISO, or UEFI boot image, and it never installs files into
`/boot`. Static-driver builds and compiled distribution keymaps are also
excluded from this initial profile.

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
requires C/C++23. Enabling `PEDIGREE_AUTO_VAR_INIT` additionally requires the
pattern and zero modes of `-ftrivial-auto-var-init`. The GCC 8.3 files in the
historical `images/local` snapshot are not a usable self-host seed.

Once those tools are available, this slice can rebuild:

- the UEFI-bootable Pedigree kernel;
- kernel modules and the deterministic initrd containing them;
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
- An Alpine x64 SDK prepared on a Linux or macOS host with Docker and copied
  into the guest. Set `PEDIGREE_TARGET_SYSROOT` to its `sysroot/` directory.
  Native configuration does not run Docker or acquire packages.
- zlib development headers and library. The native initrd builder links zlib
  directly, so no `gzip` executable is needed.
- Development headers and libraries needed by any enabled in-tree user
  applications. The Alpine desktop profile supplies their dependencies in the
  separate SDK. The default base profile leaves these applications disabled.
- See [Alpine musl SDK](musl-sdk.md) for preparing the runtime and matching
  development packages outside the guest.

## Libc and syscall boundary

Native amd64 musl uses its upstream Linux register ABI directly. Pedigree's
service-zero syscall entry translates that number inside the POSIX module, so
adding or implementing a Linux-compatible syscall does not require rebuilding
libc unless its public API also changes. Hosted builds retain a separate bridge
because a raw syscall there would enter the host operating system.

The native target consumes Alpine's installed musl headers, startup objects and
libraries without rebuilding libc. Pedigree-specific headers are provided by
the separate platform SDK.

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
    -DPEDIGREE_BUILD_UEFI=OFF \
    -DPEDIGREE_BUILD_KEYMAPS=OFF \
    -DPEDIGREE_BUILD_TRANSLATIONS=OFF
cmake --build build-boot --target boot-artifacts
```

Run `scripts/alpine/build.sh x86_64` on the cross-build host before configuration.
CMake consumes the prepared SDK without downloading or compiling libc.

The target tree owns incremental native sub-builds under `build/host-tools`.
It builds the initrd generator when needed, and adds the image utilities only
when the requested products require them. Changes to those sources or their
CMake files are picked up by the next `cmake --build build`; there is no sibling
tree to refresh or export to import.

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
    pedigree-distribution-tools pedigree-initrd-builder
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=build-etc/cmake/pedigree_amd64.cmake \
    -DPEDIGREE_HOST_TOOLS_MODE=IMPORTED \
    -DIMPORT_EXECUTABLES="$PWD/build-host/HostUtilities.cmake"
```

## First build

With the native tools installed and a prepared SDK copied into the guest, run
from the checkout:

```sh
PEDIGREE_TARGET_SYSROOT=/path/to/alpine/sysroot ./easy_build_selfhost.sh
```

The default build directory is `build-selfhost`, and the default parallelism is
one job. Useful overrides are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PEDIGREE_BUILD_DIR` | `build-selfhost` | Build directory, relative to the checkout unless absolute |
| `PEDIGREE_BUILD_JOBS` | `1` | Parallel build jobs |
| `PEDIGREE_BUILD_TYPE` | `Debug` | CMake build type |
| `PEDIGREE_NATIVE_TOOL_ROOT` | `/usr` | Prefix containing the native toolchain |
| `PEDIGREE_TARGET_SYSROOT` | `scripts/alpine/build/x86_64/sysroot` | Prepared Alpine SDK |
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
- `build-selfhost/src/user/` — built user applications and libraries;
- `build-selfhost/pedigree-c-sdk/usr/` — Pedigree-specific userspace library
  and public headers.

The self-host wrapper disables `PEDIGREE_BUILD_UEFI`, so configuring these
artifacts does not require Clang or the ext2 image utility. UEFI image packaging
is a separate cross-build step. The UEFI image places the native loader at the
removable-media fallback path and keeps its artifacts in `EFI/PEDIGREE/current` and
`EFI/PEDIGREE/known-good`. To build a GRUB-backed image with those same
variants as chainloadable menu entries, enable `PEDIGREE_BUILD_UEFI_GRUB` and
provide `grub-mkstandalone` (or the target-prefixed equivalent) with
`PEDIGREE_UEFI_GRUB_MKSTANDALONE`.

`boot-artifacts` is an aggregate build target, not an installer or staging
directory. Copying a tested kernel and initrd into a boot environment is
intentionally a separate, manual step for now.

The initrd builder uses zlib at its highest compression level and writes
deterministic gzip metadata. Cross builds compile the utility for their host,
while Pedigree builds compile it directly; neither path needs a `gzip` command.
The Python initrd implementation remains a regression oracle, not a
base-artifact dependency.

The prepared SDK uses `usr/include` and `usr/lib`, with its libc loader under
`lib`. Keep that directory layout intact when copying the SDK into the guest.

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
