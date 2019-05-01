#!/bin/bash

set +v

# Patch musl if we didn't already
if [ ! -e ".patched" ]; then
    patch -p1 <"$SRCDIR/compilers/pedigree-musl.patch"
    touch .patched
fi

# Fix executable path as compilers are most likely not present in $PATH right now.
export PATH="$SRCDIR/compilers/dir/bin:$PATH"

cp "$SRCDIR/src/modules/subsys/posix/musl/glue-musl.c" src/internal/pedigree-musl.c
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_arch.h" arch/arm/syscall_arch.h

# Remove the internal syscall.s as we implement it in our glue.
rm -f src/internal/arm/syscall.s

# Remove default signal restore (but we should add one of our own).
rm -f src/signal/arm/restore.s

# Remove some .s implementations that have .c alternatives.
rm -f src/thread/arm/{clone,__unmapself,__set_thread_area}.s

# Custom syscall_cp to use Pedigree's syscall mechanism.
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_cp-arm.musl-s" src/thread/arm/syscall_cp.s

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

CPPFLAGS="-I$SRCDIR/src/modules/subsys/posix/syscalls -I$SRCDIR/src/system/include -DARM_COMMON=1" \
CFLAGS="-O2 -g3 -ggdb -fno-omit-frame-pointer -mcpu=cortex-a8 -mtune=cortex-a8 -mfpu=vfp -mabi=aapcs -mapcs-frame" CROSS_COMPILE="$COMPILER_TARGET-" \
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
