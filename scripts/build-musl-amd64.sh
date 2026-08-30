#!/bin/bash

# Fix executable path as cross-tools are most likely not present in PATH.
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-$SRCDIR/compilers/dir}
export PATH="$toolchain_root/bin:$PATH"

build_lock="$(pwd -P)/.pedigree-build-musl.lock"
stage_root=
previous_target=
preserve_stage=0
lock_held=0
publication_started=0
publication_complete=0

cleanup_build()
{
    if [ "$publication_started" -eq 1 ] && \
        [ "$publication_complete" -eq 0 ] && \
        [ -n "$previous_target" ] && \
        { [ -e "$previous_target" ] || [ -L "$previous_target" ]; }; then
        if [ ! -e "$TARGETDIR" ] && [ ! -L "$TARGETDIR" ]; then
            mv "$previous_target" "$TARGETDIR" 2>/dev/null || preserve_stage=1
        else
            # A signal can arrive between publishing the new tree and recording
            # completion. Keep the previous tree available for recovery.
            preserve_stage=1
        fi
    fi
    if [ "$preserve_stage" -eq 0 ] && [ -n "$stage_root" ] && \
        [ -d "$stage_root" ]; then
        rm -rf "$stage_root"
    fi
    if [ "$lock_held" -eq 1 ]; then
        rmdir "$build_lock" 2>/dev/null || true
    fi
}
trap cleanup_build EXIT
trap 'exit 1' HUP INT TERM

if ! mkdir "$build_lock" 2>/dev/null; then
    echo "Another musl build is already using this source tree: $build_lock" >&2
    exit 1
fi
lock_held=1

apply_source_patch()
{
    source_patch=$1
    if patch -f -R --dry-run -s -p1 -i "$source_patch" >/dev/null 2>&1; then
        echo "Source patch already applied: $(basename "$source_patch")"
    elif patch -f --dry-run -s -p1 -i "$source_patch" >/dev/null 2>&1; then
        patch -f -s -p1 -i "$source_patch" || return 1
    else
        echo "Could not apply source patch: $source_patch" >&2
        return 1
    fi
}

apply_source_patch \
    "$SRCDIR/build-etc/toolchain/musl-1.2.6-cve-2026-40200-qsort.patch" || exit 1
apply_source_patch \
    "$SRCDIR/build-etc/toolchain/musl-1.2.6-cve-2026-6042-iconv.patch" || exit 1

cp "$SRCDIR/src/modules/subsys/posix/musl/glue-musl.c" src/internal/pedigree-musl.c
cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_arch.h" arch/x86_64/syscall_arch.h

case "$ARCH_TARGET" in
    HOSTED)
        clone_source=clone-hosted-amd64.musl-s
        # Hosted page geometry is supplied through AT_PAGESZ at runtime.
        : >arch/x86_64/bits/limits.h
        ;;
    X64)
        clone_source=clone-amd64.musl-s
        printf '#define PAGESIZE 4096\n' >arch/x86_64/bits/limits.h
        ;;
    *)
        echo "Unsupported amd64 musl architecture target: $ARCH_TARGET" >&2
        exit 1
        ;;
esac
cp "$SRCDIR/src/modules/subsys/posix/musl/$clone_source" \
    src/thread/x86_64/clone.s

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

case "$TARGETDIR" in
    /*/)
        TARGETDIR=${TARGETDIR%/}
        ;;
    /*)
        ;;
    *)
        echo "musl TARGETDIR must be an absolute path: $TARGETDIR" >>musl.log
        cat musl.log >&2
        exit 1
        ;;
esac

if [ "$TARGETDIR" = / ]; then
    echo "Refusing to install musl over the filesystem root." >>musl.log
    cat musl.log >&2
    exit 1
fi

target_parent=$(dirname "$TARGETDIR")
target_name=$(basename "$TARGETDIR")
mkdir -p "$target_parent" >>musl.log 2>&1 || {
    cat musl.log >&2
    exit 1
}

# Use a sibling temporary directory so publication and rollback stay on the
# same filesystem as the final sysroot.
stage_root=$(mktemp -d "$target_parent/.${target_name}.musl-install.XXXXXX") || {
    echo "Could not create a musl staging directory in $target_parent." \
        >>musl.log
    cat musl.log >&2
    exit 1
}
staged_target="$stage_root$TARGETDIR"

die()
{
    cat musl.log >&2; exit 1;
}

case "${PEDIGREE_TARGET_PAGE_SIZE:-}" in
    '' | *[!0-9]*)
        echo "PEDIGREE_TARGET_PAGE_SIZE must be a positive integer." >&2
        exit 1
        ;;
    0)
        echo "PEDIGREE_TARGET_PAGE_SIZE must be a positive integer." >&2
        exit 1
        ;;
esac

musl_ldflags="-Wl,-z,max-page-size=$PEDIGREE_TARGET_PAGE_SIZE -Wl,-z,common-page-size=$PEDIGREE_TARGET_PAGE_SIZE"
musl_dtrelr=0
case "${PEDIGREE_DTRELR:-OFF}" in
    1 | [Oo][Nn] | [Tt][Rr][Uu][Ee] | [Yy][Ee][Ss] | [Yy])
        musl_ldflags="$musl_ldflags -Wl,-z,pack-relative-relocs"
        musl_dtrelr=1
        ;;
esac

config_include_dir=${PEDIGREE_CONFIG_INCLUDE_DIR:-$SRCDIR/build}
CPPFLAGS="-I$SRCDIR/src/modules/subsys/posix/syscalls -I$SRCDIR/src/system/include -I$config_include_dir -D$ARCH_TARGET=1" \
CFLAGS="-O2 -g3 -ggdb -fno-omit-frame-pointer -fPIC" CROSS_COMPILE="$COMPILER_TARGET-" \
LDFLAGS="$musl_ldflags" \
../configure --target=$COMPILER_TARGET --prefix="$TARGETDIR" \
    --syslibdir="$TARGETDIR/lib" --enable-shared \
    >>musl.log 2>&1 || die

make >>musl.log 2>&1 || die
make install DESTDIR="$stage_root" >>musl.log 2>&1 || die

# Refuse to install a libc that silently selected the generic -ENOSYS fallback
# or the wrong target's trampoline.
clone_disassembly=$(
    "$COMPILER_TARGET-objdump" -d --disassemble=__clone \
        "$staged_target/lib/libc.so" 2>>musl.log
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

if [ "$musl_dtrelr" -eq 1 ] && \
    ! "$COMPILER_TARGET-readelf" -d "$staged_target/lib/libc.so" 2>>musl.log | \
        grep -q '(RELR)'; then
    echo "DT_RELR was requested but the staged libc does not contain it." >>musl.log
    die
fi

# Replace the complete sysroot only after validating it. Replacing the tree,
# instead of installing over it, also removes files left by older musl builds.
previous_target="$stage_root/previous"
had_previous=0
publication_started=1
if [ -e "$TARGETDIR" ] || [ -L "$TARGETDIR" ]; then
    mv "$TARGETDIR" "$previous_target" >>musl.log 2>&1 || die
    had_previous=1
fi

if ! mv "$staged_target" "$TARGETDIR" >>musl.log 2>&1; then
    echo "Could not publish the staged musl sysroot." >>musl.log
    if [ "$had_previous" -eq 1 ]; then
        if [ ! -e "$TARGETDIR" ] && [ ! -L "$TARGETDIR" ]; then
            if ! mv "$previous_target" "$TARGETDIR" >>musl.log 2>&1; then
                preserve_stage=1
                echo "Rollback failed; the previous sysroot remains at $previous_target." \
                    >>musl.log
            fi
        else
            preserve_stage=1
            echo "The previous sysroot remains at $previous_target." >>musl.log
        fi
    fi
    die
fi
publication_complete=1
