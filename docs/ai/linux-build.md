# Native Linux x64 build

Run these commands from the Pedigree checkout on an x86-64 Linux host. This
builds the target kernel and boot image, separately from the Linux-hosted kernel.

Easy Build requires `uv`, a running Docker engine, and the host dependencies
installed by `scripts/easy_build_deps.sh`. It prepares Alpine's runtime and SDK,
then checks or bootstraps GCC 15.3 with the existing Pedigree target patches.
There is no dependency on the sibling `pedigree-apps` repository.

```sh
PEDIGREE_BUILD_JOBS=8 ./easy_build_x64.sh
```

The default profile is a small Alpine image with `/sbin/init` and a serial getty.
It excludes Pedigree's optional userspace programs and window manager. Log in as
`root` with an empty password. Subsequent development builds use:

```sh
cmake --build build -j8
```

For the optional desktop, provide a sibling `pedigree-winman` checkout and host
`protoc` compatible with the selected Alpine protobuf development package:

```sh
PEDIGREE_ALPINE_PROFILE=desktop PEDIGREE_BUILD_JOBS=8 ./easy_build_x64.sh
```

Alpine outputs live under `scripts/alpine/build/x86_64`. Set
`PEDIGREE_ALPINE_ROOT` to choose another output directory, or
`PEDIGREE_TARGET_SYSROOT` to use a separately prepared SDK. The helper reuses
unchanged preparation outputs and a compiler matching the installed recipe.
See [Alpine musl SDK](musl-sdk.md) for all three supported architectures.

Compiler initialization of otherwise uninitialized automatic storage is off by
default. Enable it with `cmake -S . -B build -DPEDIGREE_AUTO_VAR_INIT=ON` for
debugging: Debug builds use a pattern; other build types use zeros.

The boot image is `build/pedigree-uefi.img`. Its root filesystem is
`build/pedigree-uefi-root.img`; the kernel and module archive are under
`build/src/system/kernel` and `build/src/modules`. Root image assembly copies
the prepared Alpine image and adds only declared build artifacts and selected
configuration. It preserves the base runtime and APK database, and grows the
filesystem to accommodate the overlay. Old `images/local` packages are ignored.

## Existing checkouts

An existing `.easy_os` marker skips dependency installation. Check missing tools
explicitly after moving hosts. UEFI packaging needs `clang`, `gettext-tools`,
`mtools`, `dosfstools` and `e2fsprogs`, as well as the compiler prerequisites.
Docker must be available for initial Alpine preparation.

Easy Build refreshes CMake's compiler metadata when the toolchain or SDK changes.
The bootstrap activates `compilers/dir` only after the C++ runtime validates:

```sh
compilers/dir/bin/x86_64-pedigree-gcc -dumpfullversion
readlink -f compilers/dir
```

## One-CPU boot checkpoint

With QEMU and OVMF installed:

```sh
uv run python scripts/run-qemu-uefi.py \
    --ovmf /usr/share/qemu/ovmf-x86_64-4m.bin \
    --log-dir /tmp/pedigree-uefi-checkpoint
```

This uses a disposable disk snapshot and copied firmware. Its default markers
prove early kernel and initrd startup. Add
`--require-marker "Invoking userspace program at"` to check the init handoff;
this does not validate an interactive login or desktop session.

For an interactive serial login, use `scripts/qemu --serial`. Stop the guest
before rebuilding its backing images.
