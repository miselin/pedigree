# C++ lifetime API layers

Pedigree's normal thread exits now return through C++ scopes. Lifetime APIs can
therefore use ordinary RAII for ordinary execution; explicit stack discard is
an exceptional containment path, not a second destructor system.

The API should become more opinionated as code moves away from the scheduler
and interrupt mechanisms:

1. **Mechanism layer.** Scheduler, wait, IRQ, DMA, and TLB code keeps raw,
   allocation-free operations with explicit synchronization. It must not gain
   hidden blocking or last-release deletion.
2. **Lexical ownership layer.** Subsystems use move-only owners for resources
   acquired and released within ordinary C++ control flow. `UniqueResource`
   stores one pointer and a stateless release policy; it adds no allocation,
   vtable, refcount, or dynamic deleter.
3. **Concurrent escape layer.** Work crossing a thread, queue, callback, or
   registry boundary carries an owner, registration, lease, or admission
   ticket. `AdmittedThread` starts a detached kernel worker behind an
   `OperationBarrier`, and releases admission from kernel text only after its
   possibly unloadable entry point has returned. Its scheduler-owned start
   record also retires the parameter if cancellation wins before entry begins.
4. **Active-object layer.** Shutdown remains named and ordered: stop admission,
   wake workers, drain callbacks and work, join workers, then destroy state and
   unload code. Destructors may enforce or backstop that protocol, but must not
   hide an unsafe blocking drain.
5. **Hard-context layer.** IRQ-facing paths keep preallocated generations,
   hazards, and pins. They do not allocate, wait, or become the context that
   performs final deletion.

Plain `T*` and `T&` remain appropriate for local borrows. A `Borrow<T>` wrapper
would document intent but cannot enforce non-escape with the current toolchain,
so the first ratchet targets actual acquisition and publication boundaries.

## Spike

The status server is the first outer-ring conversion. Listener connections and
received buffers have lexical owners. Each client context transfers through
`AdmittedThread`; module teardown cannot finish draining client work until the
client has released its resources, returned through all module frames, and
reached the kernel trampoline. The listener is an `OwnedThread` with explicit
stop publication and join before the client barrier drains; admission does not
pin the producer's own module frame. The only stack-discard cleanup left is the
narrow registration that publishes a stack `Completion` to the lwIP callback.

The POSIX datagram send path is a second `UniqueResource` consumer and fixes
the prior error-path `netbuf` leak. This is enough repetition to validate the
one-word owner without attempting a universal lifetime framework.

## Migration ratchet

`lifetime-escape-inventory.json` and `audit-lifetime-escapes.py` establish a
small, high-confidence baseline. The initial inventory covers 26 legacy
`PointerGuard` expressions, 9 first-party lwIP ownership-producing calls, 10
`detach` or `startDetached` publications (including one explicitly classified
non-thread API), and the two declaration/definition sites of the legacy
`runConcurrently` surface.
Every file count must match its reviewed baseline. A migration updates the
inventory in the same change, so removed debt cannot silently grow back.

This is a four-pattern pilot, not a claim that all lifetime debt is captured.
Callback registrations, queue payloads, physical-page rollback, BIOS buffers,
framebuffer handles, alarms, other manual C cleanup pairs, and permanent
handlers still need a symbol-aware inventory. The regex gate prevents the
clearest debt from growing now; a Clang AST audit should replace it when the
compiler tooling can reliably identify enclosing symbols and ownership
transfers. The current metadata records reviewed intent but cannot prove that a
same-file call remains wrapped.

New inventory entries require an explicit classification and, where relevant,
the owner or drain protocol. Prefer migrating an existing site over adding an
allowance.
