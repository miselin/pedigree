# Process creation

Use `posix_spawn` or `posix_spawnp` when a child immediately runs another program.
They support file actions and process attributes while using the same temporary
shared-address-space path as `vfork`. The parent resumes when the child commits to
exec or exits. Failed setup or exec is reported by the spawn call.

| Entry point | Behavior |
| --- | --- |
| `vfork()` | Borrow the parent's address space until exec or exit |
| `posix_spawn()`, `posix_spawnp()` | Use the vfork path, including file actions and attributes |
| `system()`, `popen()` | Use `posix_spawn()` to run the shell |
| `clone()`, `clone3()` with `CLONE_VM | CLONE_VFORK` and `SIGCHLD` | Use the vfork path |
| `fork()` and process clone without sharing flags | Create an isolated address space |
| pthread creation | Share the process as a thread; do not suspend its creator |

The target musl already selects `CLONE_VM | CLONE_VFORK | SIGCHLD` for spawn.
Programs that request ordinary fork retain fork semantics. Whether a compiler
or build tool selects spawn, vfork, or fork depends on its own configuration.

The supported raw vfork clone flags can also include `CLONE_PARENT_SETTID`,
`CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, `CLONE_SETTLS`, and `CLONE_NEWUTS`.
Musl's public `clone()` wrapper rejects explicit TLS and clear-TID flags to
protect its thread state; those modifiers are available through the raw syscalls.
`clone3` additionally supports `CLONE_CLEAR_SIGHAND`, resetting inherited signal
handlers while preserving ignored signals. Its stack argument is the lowest
address plus a size, whereas classic clone takes the initial stack pointer.

Unsupported sharing, namespace, PID-selection, and cgroup requests fail rather
than silently receiving different semantics. `CLONE_VFORK` without `CLONE_VM`
and vfork requests with a termination signal other than `SIGCHLD` are not
supported. The hosted libc's generic `vfork` fallback remains ordinary fork;
these target guarantees concern the raw Linux syscall ABI used on Pedigree.
