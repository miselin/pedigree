# IRQ and interrupt closure

Status: closed on 2026-08-09.

This closes the interrupt work that began with RTC IRQ8 starvation and the
later wait-state reduction. The interrupt architecture is ready to support
further concurrency work, with the verification boundaries below kept in the
regression suite.

## Contract

- Device hard-IRQ context may acknowledge hardware, update atomic state, and
  publish deferred work. It may not schedule, wait, acquire a semaphore, use a
  wait queue, or allocate/free heap memory.
- `HardIrqHandler` is the explicit hard-only interface.
- `IrqHandler` uses threaded delivery. `SplitIrqHandler` has a mandatory hard
  top half and a waitable threaded bottom half.
- A threaded level-triggered PIC line stays masked until its published work is
  complete. The RTC reads status register C and advances time in that threaded
  callback.
- Scheduler-timer signal entry may request a switch, but thread exit and final
  termination occur on the ordinary return tail rather than on the signal
  stack.

## Evidence

| Layer | What it proves | Closure gate |
| --- | --- | --- |
| Native tests | PIC state transitions, RTC elapsed-time aggregation, alarm ownership, and time conversion | Includes `PicContentionActions.CurrentRtcThreadedEntrySurvivesControllerOwner` |
| Native Linux hosted kernel | Real kernel threads, signals, scheduler return tails, split-handler lifecycle and orphan drain, hard-context guards, unregister drains, and interrupt-manager mutation contention | `HOSTED-IRQ-CLOSURE: PASS all` |
| QEMU UP | PC RTC calibration, PIC path, interrupt enable, and at least one second of IRQ8-backed clock progress | Historical ISO checkpoint with RTC progress enabled |
| QEMU SMP | The same PC/RTC path while four processors start and remain live long enough for IRQ8-backed clock progress | Historical ISO checkpoint with RTC progress enabled |

The hosted kernel deliberately does not emulate a PC interrupt controller or
RTC. QEMU owns those hardware-shaped checks and the real SMP check.

## Reproduce

Run the hosted closure lane directly on an x86-64 Linux host:

```sh
scripts/verify-irq-closure.sh
```

There is no Docker fallback on macOS. Use `./verify.sh` there for the maintained
native and Darwin-hosted checks. Complete Linux configure, build, test, and
runtime logs are retained in `build-verify/irq-closure`; its build utilities
are compiled and executed on that same Linux host.

Build a fresh UEFI image, then check PC startup with one and four CPUs:

```sh
cmake --build build --target uefi-image --parallel 6
uv run scripts/run-qemu-uefi.py --cpus 1 \
  --image build/pedigree-uefi.img --root build/pedigree-uefi-root.img \
  --log-dir build-verify/irq-closure/qemu-up \
  --require-marker 'TSC calibration:'

uv run scripts/run-qemu-uefi.py --cpus 4 \
  --image build/pedigree-uefi.img --root build/pedigree-uefi-root.img \
  --log-dir build-verify/irq-closure/qemu-smp \
  --require-marker 'TSC calibration:' \
  --require-marker 'Processor #3 started.'
```

These commands validate UEFI boot, RTC calibration and application-processor
startup. The current marker-based runner does not measure elapsed RTC time.
The historical IRQ8 progress checkpoint required at least one full second of
guest log timestamp advancement after interrupts were enabled; the boot checks
above do not replace that evidence.

## Boundary

This is closure on the interrupt execution model and the known RTC/scheduler
failure modes. It is not a claim that every future driver is race-free. New
interrupt-driven drivers must still choose an explicit delivery policy and add
a focused lifetime or contention regression when they introduce a new
concurrency pattern.
