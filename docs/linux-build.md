# Native Linux x64 build

Run these commands from the Pedigree checkout on an x86-64 Linux host. This
builds the target kernel and userspace; it is separate from the experimental
Linux-hosted kernel.

The full Easy Build path requires `uv` on `PATH`. Its current PUP wrapper also
expects the `pedigree-apps` checkout beside this repository, with PUP under
`../pedigree-apps/pup`.

When the sibling `pedigree-winman` checkout is present, the default build also
builds the desktop. It needs host `protoc` on `PATH`; use version 35.0 to match
the current target protobuf package. The upstream
[Linux x86-64 release](https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protoc-35.0-linux-x86_64.zip)
can be extracted into a user-owned directory with its `bin` directory on `PATH`.
Easy Build installs the target protobuf package separately through PUP.

```sh
MAKEFLAGS=-j8 ./easy_build_x64.sh
```

The first run bootstraps GCC 15.3 and installs the userspace packages. Subsequent
development builds use:

```sh
cmake --build build -j8
```

The default boot image is `build/pedigree-uefi.img`. Its root filesystem is
`build/pedigree-uefi-root.img`; the kernel and module archive are under
`build/src/system/kernel` and `build/src/modules`.
The root image grows to fit its manifest, with a 2 GiB minimum and space for
filesystem metadata and later package changes.

## Existing openSUSE checkouts

An existing `.easy_os` marker skips dependency installation, even after the host
distribution changes. Check missing tools explicitly rather than treating that
marker as proof that dependencies are available. UEFI packaging needs `clang`,
`gettext-tools`, `mtools`, `dosfstools`, and `e2fsprogs` in addition to the
compiler prerequisites installed by `scripts/easy_build_deps.sh`.

Old and new staging layouts may coexist: a file under `images/local/usr/bin`
takes precedence over the same translated name under `images/local/applications`.
The image builder preserves the staging tree and emits each such destination
once; build artifacts and base-image files retain their existing priority.

The bootstrap activates `compilers/dir` only after the final C++ runtime passes
validation. An old GCC 8 link can remain there after a failed bootstrap. Confirm
the selected version after Easy Build succeeds:

```sh
compilers/dir/bin/x86_64-pedigree-gcc -dumpfullversion
readlink -f compilers/dir
```

GCC may install its runtime under `lib64`; validation queries the compiler for
that path. Image creation also searches `/usr/sbin` and `/sbin`, which may be
absent from an ordinary SSH user's `PATH`.

## One-CPU boot checkpoint

With QEMU and openSUSE's `qemu-ovmf-x86_64` package installed:

```sh
uv run python scripts/run-qemu-uefi.py \
    --ovmf /usr/share/qemu/ovmf-x86_64-4m.bin \
    --log-dir /tmp/pedigree-uefi-checkpoint
```

This uses a disposable disk snapshot and copied firmware. Its default markers
prove early kernel and initrd startup, not a userspace benchmark. Keep timing
runs separate from this checkpoint and from concurrent compiler builds.

For a KVM checkpoint, pass `--qemu /path/to/executable-wrapper` with a wrapper
containing:

```sh
#!/bin/sh
exec qemu-system-x86_64 -accel kvm -cpu host "$@"
```

Add `--require-marker "Invoking userspace program at"` to check that boot reaches
the handoff to init. This still does not validate an interactive desktop session.
