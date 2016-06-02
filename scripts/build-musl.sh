#!/bin/bash

# Fix executable path as compilers are most likely not present in $PATH right now.
export PATH="$SRCDIR/compilers/dir/bin:$PATH"

cp "$SRCDIR/src/subsys/posix/musl/glue-musl.c" src/internal/pedigree-musl.c
# TODO: architecture hardcoded here
cp "$SRCDIR/src/subsys/posix/musl/syscall_arch.h" arch/x86_64/syscall_arch.h

rm -rf build
mkdir -p build
cd build

date >musl.log 2>&1

die()
{
    cat musl.log >&2; exit 1;
}

CPPFLAGS="-I$SRCDIR/src/subsys/posix/syscalls -I$SRCDIR/src/system/include -D$ARCH_TARGET" \
../configure --target=$COMPILER_TARGET --prefix="$TARGETDIR" \
    --syslibdir="$TARGETDIR/lib" --enable-shared \
    >>musl.log 2>&1 || die

make >>musl.log 2>&1
make install >>musl.log 2>&1
