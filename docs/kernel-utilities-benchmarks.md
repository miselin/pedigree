# Kernel utility benchmarks

The host benchmark executable is built from the same utility sources used by
the host tests. Use `benchmarker-native` for measurements: it is compiled at
`-O3` with the host CPU tuning flags. The `benchmarker` target is retained for
target-shaped builds and is not the preferred performance lane.

## Build and run

From the repository root:

```sh
cmake --build build-native/tools --target benchmarker-native -j2
build-native/tools/src/buildutil/benchmarker-native \
  --benchmark_min_time=100ms \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

Use a filter while iterating, for example:

```sh
build-native/tools/src/buildutil/benchmarker-native \
  --benchmark_filter='BM_(Tree|HashTable|List|Vector|String|RadixTree|RangeList|Buffer|RingBuffer|SlamAllocator|PMM|VMM)'
```

Keep the raw console or JSON output with the benchmark run. CPU frequency,
power state, background load, compiler flags, and allocator state can all move
small results, so compare runs from the same host and configuration.

## Coverage

The existing benchmark sources cover `Tree`, `String`, `RadixTree`, `Vector`,
`List`, `HashTable`, `RangeList`, and `SlamAllocator`. The suite also includes
standard-library comparison cases for the tree, hash, list, vector, and
allocator paths. `bench-Buffer.cc` and `bench-RingBuffer.cc` cover fixed-size
write/read cycles and compare them with a `std::deque` payload queue.

`bench-ProcessorMemory.cc` adds two levels of processor-memory evidence:

- `BM_PMM_*` measures the real `RangeList` implementation using contiguous and
  fragmented page-shaped ranges. This isolates the range-selection work used
  by the PMM without requiring a boot-time singleton or physical backing file.
- `BM_VMM_*` measures a host-only four-level page-table model with the same
  9-bit index progression and 4 KiB leaf layout as the x64 implementation. It
  measures pointer-chasing lookup and map/unmap shape, and includes an
  `std::unordered_map` lookup reference.

The RadixTree benchmark uses `/usr/share/dict/words` when available and falls
back to a generated 4096-key corpus, so the benchmark list is usable on hosts
without the optional system dictionary.

The VMM model deliberately does not claim to measure TLB invalidation,
shootdown, locking, physical page-table allocation, or scheduler interaction.
Those require a target or a much larger host adapter. Likewise, the PMM cases
do not include page-stack bookkeeping, bitmap checks, memory pressure, or
physical I/O. Use these probes to identify data-structure costs first; treat
the resulting ranking as a prompt for a second, lifecycle-level benchmark.
