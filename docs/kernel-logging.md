# Kernel log signal

Keep the default log useful for a short debugger history: device identity and
configuration, module names and address ranges, mount results, boot milestones,
and actionable failures. Successful per-event operations and raw format dumps
should not displace those messages. Expected syscall errors belong in syscall
tracing rather than unconditional kernel notices.

`PEDIGREE_DEBUG_LOGGING` is enabled in development builds. Moving a noisy notice
to `DEBUG_LOG` therefore does not remove it from those builds.

Detailed traces remain available through these compile-time controls:

- `VERBOSE_ELF=1` enables the dynamic-tag, allocation and symbol-table notices
  in `src/system/kernel/linker/Elf.cc`. It defaults to zero. Unrecognized tags
  still produce errors; known metadata skipped by that parser does not.
- `CACHE_TRACE_WRITEBACK=1` enables per-page writeback admission messages in
  `src/system/kernel/utilities/Cache.cc`. Each message identifies the cache,
  its key and the separate memory address. Disk-cache keys are byte offsets;
  this message describes queueing, not successful persistence.
- `POSIX_LOG_FACILITIES` bit 0 enables file/terminal syscall tracing, including
  terminal flag changes.

Input events without a consumer are normal for unused mouse/raw-key streams.
Missing translated-key consumers and failed delivery still produce warnings.
