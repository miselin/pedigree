# Batched filesystem sync

The ext2/AHCI `sync()` path now claims dirty cache pages in bounded groups,
submits independent writes in NCQ waves, and makes each cache group durable
before retiring its dirty generations. It also skips unchanged checksum-tracked
block-cache pages during the batch drain. An immediate clean sync still issues a
final device barrier, but no payload writes.

The full-page ext2 fast path covers 4 KiB filesystem blocks. Partial file pages,
smaller filesystem blocks, shifted disk-cache alignment, overlapping requests,
and existing lower-cache aliases retain their ordinary merge/transfer paths.
Legacy transports remain sequential. Existing xattr and quota ordering barriers
are retained; the filesystem still drains file data before its final disk-cache
sync. This is a bounded optimization, not a new writeback scheduler.

Cache snapshot pins and the existing batch ownership protocol protect page
lifetime. Every issued AHCI command is reaped on success or partial failure. A
failed transfer or durability barrier leaves the batch retryable; successful
completion records only the submitted generation, preserving later mutations.
The original forced single-page/legacy sync behavior is unchanged.

## QEMU results

One unthrottled A/B run, one vCPU, QEMU 11.0.3 TCG, Sandy Bridge model, AHCI,
4 KiB ext2, and an existing 1 MiB file. Setup reads/writes are outside the measured
`sync()` intervals. Background writeback remains enabled.

| Operation | Before | After | ATA flushes before → after | Payload writes before → after |
| --- | ---: | ---: | ---: | ---: |
| Dirty sync | 287.4 ms | 73.2 ms | 288 → 6 | 287 → 257 |
| Immediate clean sync | 22.5 ms | Below guest clock resolution | 31 → 1 | 30 → 0 |
| Rewrite and sync | 229.5 ms | 24.5 ms | 287 → 6 | 286 → 257 |

The extra candidate payload write is filesystem metadata. Unthrottled observed
NCQ depth reached 2 with one vCPU and 3 with four vCPUs: the host completes writes
quickly relative to emulated command submission. In a separate four-vCPU
qualification capped at 1000 backing-write IOPS, observed depth reached **31**
for both dirty phases. This cap demonstrates concurrent outstanding writes; its
timings are not used in the speed comparison. Deterministic admission tests fill
all 32 slots and cover saturated queues, reduced depths, admission failures,
completion failures, and the non-NCQ fallback for reads and writes.

The final unthrottled four-vCPU run passed at 96.4 ms dirty, 1.2 ms clean, and
43.4 ms redirty, again with 6/1/6 flushes and zero clean payload writes. These
small TCG samples establish less work and concurrent submission, not physical
SSD throughput or a statistically established speedup.

## Validation and reproduction

- Isolated `cmake --build ... --target kernel initrd -j 6`: pass.
- Native Cache/Partition suite: 23 tests pass, including checksum-clean skipping,
  unmarked writable aliases, failed batches, and mutations during writeback.
- AHCI admission/completion tests: two tests pass with ASan/UBSan, exercising both
  transfer directions and interrupt/polling completion paths.
- Benchmark runner tests: 14 pass.
- Fresh-kernel QEMU runs: one-vCPU A/B, four-vCPU saturation, and final
  unthrottled four-vCPU runs pass.
- Fresh four-vCPU reboot verifies every persisted file byte before further
  writes; the repeated workload passes and clean sync again writes no payload.

Use [the sync benchmark instructions](../scripts/benchmarks/README.md#filesystem-wide-sync)
for image preparation and the fresh-guest persistence check. Full commands,
immutable payload hashes, QEMU traces, reports, and the comparison JSON are under
`/private/tmp/pedigree-sync-spike`. The runtime build uses the proven-console
source configuration from the preceding T420 work. The concurrent main-task
musl packaging changes are not part of this patch.

The first benchmark attempt stopped before measurement because the raw serial
device does not support the terminal ioctl; the benchmark now treats that setup
as optional, matching the existing runner fixture. Its failed log is retained.
The initial isolated build inherited an old host-tools build path and failed
CMake's source-directory check; subsequent builds use spike-owned directories.
