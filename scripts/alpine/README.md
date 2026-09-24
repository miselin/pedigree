# Alpine Userspace on Pedigree

Prepare an Alpine 3.22 root filesystem and matching development sysroot with:

```sh
scripts/alpine/build.sh x86_64
scripts/alpine/build.sh aarch64
scripts/alpine/build.sh armv7
```

Docker must provide a Linux engine. The builder runs on the engine's native
architecture and uses apk to install signed target packages without executing
their installation scripts. No target emulation is required.

Outputs go to `scripts/alpine/build/ARCH`, or the optional second argument:

- `rootfs.img`: ext2 runtime image with Alpine's package database and ownership.
- `rootfs/`: runtime files, exported with the invoking user's ownership.
- `sysroot/`: development headers and libraries, including `/lib` and `/usr/lib`
  so musl's relative loader symlinks remain valid. Target executables in
  `/usr/bin` satisfy package metadata references; do not run them on the host.
- `sysroot.tar`: complete development sysroot, preserving case-sensitive paths.
- `rootfs.packages` and `sysroot.packages`: installed package versions.

The default `base` profile uses Alpine's minimal root filesystem and BusyBox
init, serial getty, login, and shell. Its development sysroot adds musl and Linux
headers. Development packages are excluded from the runtime image.

The x86-64 runtime and SDK retain `/usr/lib/ld-musl-x86_64.so.1` as a symlink to
Alpine's `/lib/ld-musl-x86_64.so.1` for binaries linked by the existing Pedigree
compiler. The standard cross-build selects Alpine's loader path directly for
new applications.

Use `--profile desktop` for Bash, C++ runtime libraries, graphics and font
libraries, gettext, dialog, and protobuf, with their development packages. This
profile is available on all three architectures; it does not install a desktop
session. Alpine provides the dialog command but does not provide `libdialog`.

Preparations are reused when the architecture, profile, and builder sources are
unchanged. Pass `--refresh` to fetch current packages from Alpine's 3.22
repositories. The development root permits passwordless root login on ttyS0;
Pedigree applications and configuration can be installed as a separate overlay.

On a case-insensitive host filesystem, Linux's uppercase/lowercase netfilter
header pairs cannot coexist in `sysroot/`; extraction keeps the last spelling
and reports the affected paths. The musl headers are unaffected. For projects
using those Linux headers, extract `sysroot.tar` on a case-sensitive filesystem
and select that directory as the target sysroot.

For desktop builds, `protoc.sh` runs Alpine's matching Protobuf compiler on the
Docker engine's native architecture. Set `PEDIGREE_WINMAN_SOURCE_DIR` and
`PEDIGREE_BUILD_DIR`, then invoke it from either directory. It mounts only those
directories, with the source read-only. `protoc.sh --version` needs neither.
