# Thread exit and C++ stack lifetime

## Contract

An ordinary terminal request is state, not a control transfer. It may wake an
interruptible wait, but the wait must return `Unwinding` or `Terminating` to its
caller. Each caller then releases its local ownership and returns normally.
The kernel-thread trampoline and the existing syscall/user-return boundaries
are responsible for the final no-return transition.

This is what makes C++ automatic storage reliable in the kernel: terminal
requests normally leave a scope through its ordinary return path, so local
destructors run in language-defined order.

## Final transitions

There are two deliberately different scheduler operations:

- `commitCurrentThreadExit()` is for an audited root or architectural
  boundary. Before switching stacks it rejects active deferral scopes,
  registered cleanup records, and live wait records. Calling it while an
  ordinary C++ scope still owns a resource is a bug.
- `abandonCurrentThreadStack(reason)` is exceptional recovery. It explicitly
  means that arbitrary C++ locals may leak because their destructors will not
  run. Each use supplies a reason and increments monotonic telemetry. The path
  first unlinks published wait records, then runs registered best-effort
  cleanup before final Thread shutdown.

An operation may carry a `Thread::StackDiscardScope` for external registrations
such as timer alarms and join claims. The scope covers the complete operation,
not just one WaitQueue enrolment. Its hook prevents a dead Thread pointer from
escaping into another subsystem; it does not try to reconstruct or destroy
arbitrary C++ locals.

This spike preserves the existing emergency-shutdown limitation: registered
cleanup and Thread shutdown can acquire locks or wait for in-flight work. An
unrecoverable fault reached with arbitrary locks held can therefore still
deadlock or leak. `abandonCurrentThreadStack` is not a general fault-unwinder;
turning that path into a nonblocking two-phase recovery protocol is separate
work.

The second operation is not a cancellation mechanism. New uses require an
audited reason why returning through the interrupted call chain is impossible.

## Ownership barriers

Some waits cannot return merely because termination is pending. Mutex
acquisition, joins used for destruction, and admission/drain operations must
first establish their ownership or completion predicate. They retain the
terminal request while finishing that predicate; the surrounding interruptible
operation or root boundary propagates it afterward.

In particular, a mutex wait may not report failure to a caller that assumes a
return means the mutex is owned.

## Review ratchet

- Do not terminate a thread from inside `WaitQueue` or another blocking
  primitive.
- Keep interruptible results explicit and checked.
- Let kernel workers return from their entry function after observing a
  terminal result.
- Keep cleanup needed by ordinary return in ordinary C++ ownership objects,
  not cross-thread callbacks into another thread's stack.
- Reserve registered state-cleanup records for explicit context replacement
  and exceptional stack discard.
- Add a destructor canary for any new terminal propagation path, and verify
  that an intentional discard increments the discard counter.

User context replacement (`sigreturn`, `exec`, and equivalent architecture
tails) is a separate audited operation. It may replace saved machine state, but
it is not permission to discard arbitrary kernel C++ frames.
