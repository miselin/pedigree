# Small vectored file I/O

Use the RAM-root compiler fixture from [compile-matrix.md](compile-matrix.md)
to compare changes to `readv` and `writev` without device I/O. Keep syscall
tracing, timing diagnostics and kernel serial logging disabled for timing runs.
Preserve the earlier detailed syscall trace separately.

## Link-only timing

Build and install the current `compile-matrix.c` driver in a disposable fixture.
Create `/root/compile-bench/matrix-link-only` containing `link` and append that
absolute path to `/root/compile-bench/ramroot-paths`. Keep the RAM bootstrap as
`/usr/bin/init`. Do not also install `matrix-trace`.

```sh
uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os pedigree --mode link --storage ramfs \
  --image /absolute/path/to/frozen-fixture.qcow2 \
  --firmware-code /absolute/path/to/OVMF_CODE.fd \
  --output /absolute/path/to/new-run
```

This runs one warm link and five measured links, checking all input identities,
output executables and zero block-device requests. Linux accepts the same mode
with its usual explicit boot disk/kernel/initrd arguments. The full-matrix
summarizer intentionally accepts only full matrices; use the five `r*-link`
rows in `report.json` for a link-only comparison.

Run baseline, candidate, candidate, baseline sequentially, each in a fresh
overlay, with no concurrent builds or guests. Keep both host wall and guest
wall/user/system values: serial gate overhead can affect the host measurement.
Use matching userspace fixtures and record kernel/initrd hashes for each arm.

## Vector I/O correctness

```sh
compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/vector-io-contract.c -o /absolute/path/to/vector-io-contract
```

Install this binary as `/usr/bin/init` in a separate disposable guest fixture.
It opens the serial console, mounts a fresh ramfs, and emits per-case results
followed by `VECTORIO PASS END`. Require that marker with no earlier failure,
and bound the guest's runtime. Run it on both baseline and candidate kernels.
`--stdio` instead preserves the caller's console and uses a temporary directory.

The contract covers 1/2/8/9 vectors, 16/2048/2049/8192-byte requests, positional
offsets, snapshot aliasing, zero-length vectors, inaccessible arrays/payloads,
partial read faults, write gather faults, EOF and competing 4096-byte pipe
writes. On Pedigree, the tested regular writes gather the whole chunk before
committing; the test checks that a later fault leaves the file unchanged.
Its optional Linux run accepts Linux's valid-prefix result after checking bytes
and offsets. Passing there is not a substitute for Pedigree guest execution.

Pedigree also checks eventfd writes with one 8-byte value per vector, split
eventfd/timerfd reads, queued signalfd records with partial-record guards, and
`EINVAL` from unsupported timerfd/signalfd vector writes. These descriptors are
nonblocking, and timer readiness has a two-second limit. The optional Linux run
prints an explicit `SKIP` for these Pedigree-specific contracts. Shared offsets
through `dup`, `lseek`, `readv`, and `writev` are checked on both systems.

Keep the existing bounce capacities when adding local storage. Smaller chunks
change partial-fault behavior, and splitting pipe writes can break atomicity.
Check generated stack usage with the target compiler's `-fstack-usage`; x64
kernel stacks are 32 KiB. Preserve automatic local-variable initialization when
comparing results, and include its cost in the measurement.
