# Alpine musl SDK

Target builds use Alpine's musl runtime and development packages. Prepare them
with a running Docker engine and the common helper:

```sh
scripts/alpine/build.sh x86_64
scripts/alpine/build.sh aarch64
scripts/alpine/build.sh armv7
```

The default `base` profile keeps the guest small: Alpine init, BusyBox tools and
a serial getty. Log in as `root` with an empty password. The `desktop` profile
adds dependencies for the optional x64 desktop:

```sh
scripts/alpine/build.sh x86_64 /path/to/alpine-desktop --profile desktop
```

Each output directory contains:

- `rootfs.img` and the matching `rootfs/` runtime tree, including the APK database;
- `sysroot/`, with headers and link libraries from matching development packages;
- `sysroot.tar`, preserving the SDK's case-sensitive paths; and
- `rootfs.packages` and `sysroot.packages`, recording the selected package versions.

The default output is `scripts/alpine/build/<architecture>`. Repeated preparation
reuses complete outputs when the profile and preparation recipe are unchanged.
Initial preparation needs network access; prepared artifacts can be copied to
another host for offline use. Cross compilation consumes `sysroot/`; neither
configuration nor SDK updates install libc into the running system.

CMake accepts `-DPEDIGREE_ALPINE_ROOT=/path/to/output` and
`-DPEDIGREE_TARGET_SYSROOT=/path/to/output/sysroot`. Easy Build accepts the same
names as environment variables. The cross-toolchain bootstrap takes the SDK's
`usr` directory: `--sysroot /path/to/output/sysroot/usr`.

Pedigree-specific APIs remain in `pedigree-c-sdk/usr`, including
`<pedigree/log.h>`, `<pedigree/fb.h>` and `libpedigree-c`. They are not patched into
musl. The image overlay preserves Alpine's libc, accounts, directory symlinks
and package database.

Hosted builds still compile a separate musl variant that routes syscalls through
Pedigree's in-process kernel bridge. Alpine's target libc cannot replace it.
Those rules remain in `PedigreeHostedMusl.cmake`, with the
`PEDIGREE_MUSL_ARCHIVE` source-archive override.
