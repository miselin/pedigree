#!/bin/bash

# Fix executable path as compilers are most likely not present in $PATH right now.
export PATH="$SRCDIR/compilers/dir/bin:$PATH"

cp "$SRCDIR/src/modules/subsys/posix/musl/glue-musl.c" src/internal/pedigree-musl.c
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_arch.h" arch/x86_64/syscall_arch.h

case "$ARCH_TARGET" in
    HOSTED)
        clone_source=clone-hosted-amd64.musl-s
        ;;
    X64)
        clone_source=clone-amd64.musl-s
        ;;
    *)
        echo "Unsupported amd64 musl architecture target: $ARCH_TARGET" >&2
        exit 1
        ;;
esac
cp "$SRCDIR/src/modules/subsys/posix/musl/$clone_source" \
    src/thread/x86_64/clone.s

# Remove the internal syscall.s as we implement it in our glue.
rm -f src/internal/x86_64/syscall.s

# Remove default signal restore (but we should add one of our own).
rm -f src/signal/x86_64/restore.s

# No vfork()
rm -f src/process/x86_64/vfork.s

# Keep the target-specific clone trampoline. The generic C fallback only
# returns -ENOSYS.
rm -f src/thread/x86_64/{__unmapself,__set_thread_area}.s

# Custom syscall_cp to use Pedigree's syscall mechanism.
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_cp-amd64.musl-s" src/thread/x86_64/syscall_cp.s

# Custom ttyname that doesn't use /proc
cp "$SRCDIR/src/modules/subsys/posix/musl/ttyname.c" src/unistd/ttyname_r.c

# Copy custom headers.
cp "$SRCDIR/src/modules/subsys/posix/musl/fb.h" include/sys/
cp "$SRCDIR/src/modules/subsys/posix/musl/klog.h" include/sys/

rm -rf build
mkdir -p build
cd build

date >musl.log 2>&1

die()
{
    cat musl.log >&2; exit 1;
}

CPPFLAGS="-I$SRCDIR/src/modules/subsys/posix/syscalls -I$SRCDIR/src/system/include -D$ARCH_TARGET=1" \
CFLAGS="-O2 -g3 -ggdb -fno-omit-frame-pointer" CROSS_COMPILE="$COMPILER_TARGET-" \
../configure --target=$COMPILER_TARGET --prefix="$TARGETDIR" \
    --syslibdir="$TARGETDIR/lib" --enable-shared \
    >>musl.log 2>&1 || die

# This is a very ugly hack that fixes a "Nonrepresentable section on output"
# error with GCC 6.3.0 + Binutils 2.28. It's almost certainly caused by the
# Pedigree custom target, somehow.
# TODO: fix this properly.
sed -i.bak 's/-Wl,--gc-sections//g' config.mak

make >>musl.log 2>&1 || die
make install >>musl.log 2>&1 || die

# Refuse to install a libc that silently selected the generic -ENOSYS fallback
# or the wrong target's trampoline.
clone_disassembly=$(
    "$COMPILER_TARGET-objdump" -d --disassemble=__clone \
        "$TARGETDIR/lib/libc.so" 2>>musl.log
) || die
clone_syscalls=$(printf '%s\n' "$clone_disassembly" | grep -c '[[:space:]]syscall')
case "$ARCH_TARGET" in
    HOSTED)
        if [ "$clone_syscalls" -ne 0 ] || \
            ! printf '%s\n' "$clone_disassembly" | \
                grep -q 'pedigree_translate_syscall' || \
            ! printf '%s\n' "$clone_disassembly" | \
                grep -q 'pedigree_musl_thread_exit'; then
            echo "Hosted musl __clone did not retain its syscall-bridge trampoline." \
                >>musl.log
            printf '%s\n' "$clone_disassembly" >>musl.log
            die
        fi
        ;;
    X64)
        if [ "$clone_syscalls" -lt 2 ]; then
            echo "Native musl __clone did not retain its clone and thread-exit syscalls." \
                >>musl.log
            printf '%s\n' "$clone_disassembly" >>musl.log
            die
        fi
        ;;
esac
