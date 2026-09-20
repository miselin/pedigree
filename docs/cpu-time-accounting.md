# CPU-time accounting

`PEDIGREE_TIME_ACCOUNTING` enables per-thread and per-process user/kernel CPU
totals. Precise accounting is the default. It samples a monotonic clock at
kernel transitions and scheduler boundaries, publishing elapsed time without
waiting for another timer interrupt. The x64 clock uses a calibrated TSC and
precomputed multiply/shift conversion; it does not recalibrate on every syscall.

## Sampled mode

For x64 and hosted builds, enable scheduler-tick accounting with:

```sh
cmake -S . -B build -DPEDIGREE_SAMPLED_TIME_ACCOUNTING=ON
cmake --build build --target kernel initrd -j8
```

Set the option to `OFF` to restore precise accounting. Sampling is incompatible
with `PEDIGREE_BENCHMARK_SYSCALL_TIMING`, which attributes measured intervals to
individual syscalls.

Sampled mode removes clock reads and CPU-total publication from syscall,
interrupt and context-switch boundaries. Each real scheduler tick charges its
nominal duration to the interrupted thread and its process. The saved interrupt
frame selects user or kernel time; the logical mode used by return handling
does not override that frame. Reschedule IPIs and idle threads add no time.
Publication still wakes the deferred worker for armed CPU interval timers.

The native LAPIC/PIT period is normally 10ms; hosted uses 100ms. Changing the
scheduler tick divisor does not change accounting frequency. This option does
not change wall clocks, clock calibration, or timer frequency.

## Precision limits

Sampling estimates CPU usage. Work shorter than a tick can report zero; short
syscalls can collectively consume time without receiving a kernel sample.
Periodic workloads can align with the timer and bias the reported split. Native
timer interrupts that coalesce while delivery is masked are not recovered;
hosted expiration batches are attributed to the frame present at delivery.
CPU interval timers inherit this granularity.

Saved privilege identifies the sampled mode without waiting for C++ accounting
setup, but it cannot observe entry/exit assembly while interrupts are masked.
A delayed timer delivered after SYSRET samples user mode. Sampling therefore
does not fix precise attribution of those entry/exit instructions.

The precise path also has a boundary limitation: it begins kernel accounting
inside C++ and switches back to user accounting before the final assembly
return. Those portions are currently charged as user time. Accurate attribution
of the entire kernel path requires architectural entry/exit timestamps;
switching to ticks alone does not provide it.
