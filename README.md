# The Pedigree Operating System

The Pedigree Operating System started as a project in 2008 on the [OSDev.org Forums](https://forum.osdev.org/viewtopic.php?t=15939).
A small team built the core, and since then it has received contributions from numerous others.
It is as much a research project as it is a hobby OS kernel: exploring ideas around subsystems,
syscall APIs, and kernel design, and the result is a powerful kernel with a flexible userspace.

While regular development on Pedigree has slowed down dramatically, it is still a powerful system,
with past demonstrations including:

- a publicly hosted website running in Pedigree on a VM
- a public SSH endpoint to log into a real Pedigree system and explore
- a Linux-compatible userspace subsystem that could run a Debian LiveCD's userspace from init to shell

> [!IMPORTANT]
> This repository is moving forward with modernization and AI-assisted development
> to address long-standing bugs from my early years as a programmer.
> I have preserved the original state of the repository before these newer
> developments at [miselin/pedigree-legacy](https://github.com/miselin/pedigree-legacy)
> as a reference.

---

## Building

Pedigree uses CMake and a C/C++23-capable cross-toolchain. The x86-64 target boots
through UEFI. See the [Linux build guide](docs/ai/linux-build.md) for host tools,
packages and firmware, and [build profiles](docs/ai/kernel-build-profiles.md) for
configuration options. The [self-hosting guide](docs/ai/self-hosting.md) covers
cross-build host tools and the experimental native build.

The Easy Build helper bootstraps the toolchain, configures PUP, installs target
packages and builds the system. It needs `uv` and the `pedigree-apps` checkout
beside this repository. A sibling `pedigree-winman` checkout enables the desktop.

```sh
./easy_build_x64.sh
```

With an existing toolchain and populated package staging tree, configure directly:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=build-etc/cmake/pedigree_amd64.cmake \
  -DPEDIGREE_TOOLCHAIN_ROOT=/path/to/pedigree-toolchain
cmake --build build --parallel 8
```

Subsequent builds reuse `build`. To refresh the UEFI boot image explicitly:

```sh
cmake --build build --target uefi-image --parallel 8
```

ARM64 and ARMv7 have separate `easy_build_arm64.sh` and `easy_build_armv7.sh`
helpers and build trees. `./verify.sh` runs the maintained native/hosted checks.

User test and benchmark applications are excluded from the default build.
Build them with `cmake --build build --target user-tests`, or select an individual
`app-<name>` target. To include them in the guest image, configure
`-DPEDIGREE_BUILD_USER_TESTS=ON` and rebuild `uefi-image`.

## Running Pedigree

The x86-64 build produces `build/pedigree-uefi.img` and
`build/pedigree-uefi-root.img`. With QEMU and OVMF installed, run from this checkout:

```sh
scripts/qemu --serial
```

This opens the guest serial terminal. Omit `--serial` for the graphical display.
Set `OVMF_CODE` and `OVMF_VARS` if the helper cannot find your firmware files.
Stop the guest before rebuilding either image.

The graphical boot screen shows kernel logs by default. Add `splash=image` to
the kernel command line to show the image instead. `splash=log` explicitly
selects logs; omitting the option has the same effect.

Use `video=WIDTHxHEIGHTxBPP` to request a boot display mode, such as
`video=1280x720x32`. The default is `1024x768x32`; unavailable modes use the
firmware framebuffer or the existing fallback modes. Splash colours accept
six hexadecimal RGB digits: `splash-background=000000`,
`splash-foreground=FFFFFF`, `splash-border=965000`, and `splash-fill=966400`
are the defaults. Invalid values are ignored.

## Images and packages

`images/local` is the staging tree for the root filesystem. Configure PUP for
this checkout, then sync and install packages:

```sh
uv run python setup_pup.py amd64
./run_pup.sh sync
./run_pup.sh install <package>
```

`run_pup.sh` uses the current updater from the sibling `pedigree-apps` repository.
Easy Build performs the initial configuration and package installation.

You can also add files under `images/local`, using their target paths: for
example, `images/local/home/yourname/.bashrc`. After changing staged packages or
files, remove the generated root image and rebuild so those changes are included:

```sh
rm -f build/pedigree-uefi-root.img
cmake --build build --target uefi-image --parallel 8
```

Base accounts come from `images/base/etc/passwd`, `group`, and `shadow`.
Update those files to change the accounts included in a newly built image.

Historical disk images are available in the [download archive](https://dl.pedigree-project.org).
They may use boot paths that are no longer supported by the current source.

## Reporting Issues

Report any issues on the project tracker at http://pedigree-project.org

## Contact

You can find us in #pedigree on Freenode IRC.

## Contributing

We welcome contributions. The preferred mechanism for contributing is via pull
requests. See the issue trackers at http://pedigree-project.org if you need
ideas. Alternatively, come join us in our IRC channel on Freenode (see above).

We highly recommend working through a successful build and playing with some of
Pedigree's features in a VM before leaping into contributing. This will help
with understanding much of what you see in the code, and also potentially give
you some more ideas about areas to contribute to.
