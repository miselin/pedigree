#!/bin/bash

# Fix executable path as compilers are most likely not present in $PATH right now.
export PATH="$SRCDIR/compilers/dir/bin:$PATH"

cp "$SRCDIR/src/modules/subsys/posix/musl/glue-musl.c" src/internal/pedigree-musl.c
# TODO: architecture hardcoded here
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_arch.h" arch/x86_64/syscall_arch.h

# Remove the internal syscall.s as we implement it in our glue.
rm -f src/internal/x86_64/syscall.s

# Remove default signal restore (but we should add one of our own).
rm -f src/signal/x86_64/restore.s

# No vfork()
rm -f src/process/x86_64/vfork.s

# Remove some .s implementations that have .c alternatives.
rm -f src/thread/x86_64/{clone,__unmapself,__set_thread_area}.s

# Custom syscall_cp to use Pedigree's syscall mechanism.
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_cp-x86_64.musl-s" src/thread/x86_64/syscall_cp.s

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

CPPFLAGS="-I$SRCDIR/src/modules/subsys/posix/syscalls -I$SRCDIR/src/system/include -D$ARCH_TARGET" \
CFLAGS="-O2 -g3 -ggdb -fno-omit-frame-pointer" CROSS_COMPILE="$COMPILER_TARGET-" \
../configure --target=$COMPILER_TARGET --prefix="$TARGETDIR" \
    --syslibdir="$TARGETDIR/lib" --enable-shared \
    >>musl.log 2>&1 || die

# This is a very ugly hack that fixes a "Nonrepresentable section on output"
# error with GCC 6.3.0 + Binutils 2.28. It's almost certainly caused by the
# Pedigree custom target, somehow.
# TODO: fix this properly.
sed -i.bak 's/-Wl,--gc-sections//g' config.mak

make >>musl.log 2>&1
make install >>musl.log 2>&1
