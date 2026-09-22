# musl SDK

X64 and self-hosted builds acquire the pinned `musl` PUP through CMake. The SDK
is installed into `build/musl`, including headers, CRT objects, libraries,
and the dynamic loader. The running system's libc is not replaced.

CMake finds `pup` on PATH or in the checkout's `.venv/bin`; otherwise it uses
Python's `pedigree_updater` module, or downloads the standalone PUP wheel into
the build directory. The Python runtime must have that PUP release's dependencies
(older releases require Requests). No sibling checkout is needed. Override this
with `-DPEDIGREE_PUP_COMMAND=/path/to/pup` if needed. Commands with arguments use
CMake list syntax, for example `-DPEDIGREE_PUP_COMMAND=/path/to/python;-m;pedigree_updater`.

No `pup sync` is needed: the build carries its own pinned package metadata and
checks the archive's SHA256. `PEDIGREE_PUP_SERVER` selects a mirror.
For an offline first build, put `musl-1.2.6-amd64.pup` into the directory selected
by `PEDIGREE_MUSL_PACKAGE_CACHE` (default: `build/musl-pup-cache`). PUP must either
be installed or supplied as a local wheel with `PEDIGREE_PUP_WHEEL`.
Subsequent builds reuse the SDK without network access; missing
SDK members are repaired from the cached package.

Routine target builds no longer download musl source or require `patch` to build
libc. libc changes belong in the `musl` recipe in pedigree-apps. Updating the
dependency requires updating the pinned metadata and SHA256 in
`build-etc/cmake/InstallMuslPackage.cmake`.

Offline integration checks accept the published archive and a PUP executable:

```sh
PEDIGREE_TEST_MUSL_PUP=/path/to/musl-1.2.6-amd64.pup \
PEDIGREE_TEST_PUP=/path/to/pup \
    uv run python -m unittest tests.test_musl_package -v
```

Hosted builds still compile a separate musl variant that routes syscalls through
Pedigree's in-process kernel bridge. The target PUP cannot replace it. Their
source-build rules are isolated in `PedigreeHostedMusl.cmake`; the existing
`PEDIGREE_MUSL_ARCHIVE` override remains available for that variant.
