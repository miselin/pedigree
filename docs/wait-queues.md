# Waiting and asynchronous lifetime contracts

Pedigree's blocking primitives are built on `WaitQueue`. Code outside that
implementation must not put a `Thread` to sleep or make one ready directly.

The central invariant is:

> The condition which permits progress and the publication of the waiter are
> both protected by the same `WaitQueue` guard.

This closes both forms of lost wakeup:

1. a wake between checking the condition and joining the queue; and
2. a wake after joining the queue but before the scheduler commits the
   `Sleeping` transition.

## Choosing an API

- Use `WaitQueue` when implementing a new blocking primitive. Check the
  predicate while holding its `Guard`, then call `Guard::wait()`.
- Use `Guard::waitAndUnlock()` when an existing `Mutex` protects the
  predicate. It publishes the waiter before releasing the mutex and reacquires
  the mutex before returning.
- Use `ConditionVariable` for ordinary mutex-protected conditions.
- Use `Semaphore` for counted resources. Use `acquireForCompletion()` when
  abandoning the wait would release storage still owned by asynchronous work.
- Use `Completion` for a one-way operation result.
- Use `OperationBarrier` for callback or operation registries: close admission,
  unregister the external source, and drain admitted callbacks before freeing
  their state.
- Use `TerminationDeferral` only around a scope whose stack or object state
  must survive a terminal request. It does not make an interruptible operation
  successful; it delays destruction until the ownership boundary is safe.
- Use `OwnedThread` when an object's lifetime owns a worker. Detached workers
  must not retain pointers into unloadable modules or destructible objects.

An interruptible wait needs an abandonment callback when external state must
be undone if the thread terminates at the wait boundary. A completion wait
must instead remain blocked until the operation releases its ownership.

## Required wait shape

```cpp
while (true)
{
    auto guard = waiters.acquire();
    if (predicate())
    {
        break;
    }

    const WaitQueue::WakeReason reason = guard.wait(
        WaitQueue::Channel(this), Thread::CondWait,
        reinterpret_cast<uintptr_t>(this));
    if (reason == WaitQueue::WakeReason::Terminating)
    {
        return false;
    }
}
```

The code which changes `predicate()` must take the same guard and perform the
wake before releasing it:

```cpp
{
    auto guard = waiters.acquire();
    publishState();
    guard.wakeAll(
        WaitQueue::WakeReason::Signalled,
        WaitQueue::Channel(this));
}
```

The channel is part of the predicate. Use a stable owner pointer plus a scalar
value when one queue carries independent conditions. Do not use temporary
storage as a channel owner.

## Patterns which are not valid

- Check a flag, release its lock, and then sleep.
- Release a mutex and subsequently join a condition queue.
- Call `schedule(Thread::Sleeping, ...)`, `blockCurrent()`, or set
  `Thread::Sleeping` outside `WaitQueue`.
- Treat a wake as proof that the predicate is true. Every wait is a loop.
- Return from a synchronous wrapper while a controller, callback, or worker can
  still touch a caller-owned buffer.
- Remove a callback pointer without first closing admission and draining a
  callback which already copied it.
- Detach a worker which refers to its creating object or module.
- Let signal or terminal delivery bypass mutex reacquisition or another
  ownership barrier.
- Publish a non-trivial aggregate return type across the hosted kernel/module
  compiler boundary. Use a scalar result with an out parameter.

## Debugging in the kernel debugger

The `threads` view reports the persistent waiter record for each blocked
thread:

- `waitq` identifies the queue protecting the condition;
- `channel` identifies the particular condition on that queue;
- `reason=waiting` means no wake has won yet;
- `queued` means publication is complete;
- `publishing` identifies the narrow publication transition;
- `level` identifies the nested event state;
- `owner` is shown for a mutex-backed semaphore; and
- `INVALID: sleeping without waitq` is always a scheduler contract violation.

For a hang, capture all threads involved and answer these questions:

1. Which exact predicate is each thread waiting for?
2. Which queue guard protects each predicate?
3. Which thread or callback can change it?
4. Is that producer waiting for a lock owned by one of the consumers?
5. Did the producer change the predicate and wake under the same guard?
6. Is teardown waiting for a callback or worker whose admission is still open?

Repeated snapshots distinguish a deadlock from slow progress. An unchanged
wait queue, channel, owner, and instruction pointer identifies a stable wait
edge. A changing instruction pointer or request count indicates progress and
should not be diagnosed as deadlock merely because an operation is long.

## Deterministic hosted regressions

Do not try to reproduce a lost wakeup by running a workload until it happens.
Add a hosted-only hook at the exact publication boundary, force the competing
operation in that hook, and assert both the scheduler state and the final
predicate.

Useful adversarial windows include:

- waiter published, scheduler has not committed `Sleeping`;
- timeout callback admitted, cancellation has not removed its source;
- mutex released, condition waiter has not resumed;
- worker created, worker-owned lifetime scope not installed;
- callback copied from a registry, unregister has begun;
- terminal request delivered while a completion owner is blocked; and
- object teardown after work is queued but before the worker consumes it.

Every regression should have a unique `HOSTED-WAIT-TEST: PASS ...` marker.
The public smoke script requires the markers and rejects ASan reports, page
faults, relocation failures, wait-test failures, and incomplete shutdown.
Run the hosted matrix with both the system allocator and `SlamAllocator`;
allocator choice changes timing and exposes different ownership mistakes.

Native unit tests remain useful for state machines, reference counts, overflow,
and container invariants. They do not exercise the real scheduler. A hosted
module regression does, while remaining much faster and more controllable than
booting a PC platform in QEMU.

## Static and build-time gates

`verify.sh` rejects direct scheduler blocking/status changes outside the
implementation boundary, ambiguous mutex construction, hand-rolled volatile
locks, and exported result shapes known to vary across the two hosted
compilers. Add a gate when a bug can be described syntactically, but keep a
runtime regression as well: most lost wakeups are valid-looking local code
whose error is the relationship between a predicate, a lock, and a wake.

Compile both hosted modules and the x86-64 targets. The hosted kernel and its
dynamic modules intentionally exercise the native-kernel/Pedigree-module ABI
boundary. An x86-64 build catches architecture-specific implementations which
the hosted machine does not compile.

## What still needs QEMU or hardware

The hosted kernel is sufficient for scheduler publication, signal and timeout
interruption, lock ordering, request lifecycle, callback drains, module
loading, and most object ownership bugs.

QEMU or hardware remains necessary for:

- real interrupt-controller acknowledgement and masking;
- controller DMA visibility and cancellation;
- architecture memory-ordering differences;
- page-table and privilege-transition behaviour; and
- driver behaviour dependent on device timing.

A hosted fake can still prove the common controller contract—for example that
cancel-and-drain prevents a synchronous wrapper from returning early—but only
an architecture build plus QEMU or hardware can prove each real controller
implements that contract.
