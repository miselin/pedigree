#!/bin/bash

# Cross builds use the provisioned toolchain. A Pedigree-native build can use
# the compiler and binutils already installed in PATH.
if [ "${PEDIGREE_USE_PATH_TOOLCHAIN:-0}" != 1 ]; then
    toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-$SRCDIR/compilers/dir}
    export PATH="$toolchain_root/bin:$PATH"
elif [ -n "${PEDIGREE_NATIVE_TOOL_ROOT:-}" ]; then
    export PATH="$PEDIGREE_NATIVE_TOOL_ROOT/bin:$PATH"
fi

if [ "${PEDIGREE_CROSS_COMPILE_PREFIX+x}" = x ]; then
    cross_compile_prefix=$PEDIGREE_CROSS_COMPILE_PREFIX
else
    cross_compile_prefix="$COMPILER_TARGET-"
fi
objdump_tool=${PEDIGREE_OBJDUMP:-${cross_compile_prefix}objdump}
readelf_tool=${PEDIGREE_READELF:-${cross_compile_prefix}readelf}

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

case "$TARGETDIR" in
    /*/)
        TARGETDIR=${TARGETDIR%/}
        ;;
    /*)
        ;;
    *)
        echo "musl TARGETDIR must be an absolute path: $TARGETDIR" >&2
        exit 1
        ;;
esac

if [ "$TARGETDIR" = / ]; then
    echo "Refusing to install musl over the filesystem root." >&2
    exit 1
fi

# The libc target invokes this script on every build so damage to any SDK
# member is repaired even when CMake's primary output remains present.
if [ -d "$TARGETDIR" ] && \
    "$CMAKE_COMMAND" \
        "-DMANIFEST=$TARGETDIR/usr/share/pedigree/libc/manifest.json" \
        "-DSDK_ROOT=$TARGETDIR" \
        "-DEXPECTED_BUILD_ID=$PEDIGREE_MUSL_BUILD_ID" \
        -P "$PEDIGREE_MUSL_MANIFEST_VALIDATOR" >/dev/null 2>&1; then
    exit 0
fi

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

upstream_snapshot=.pedigree-upstream/x86_64
mkdir -p "$upstream_snapshot"
if [ ! -f "$upstream_snapshot/syscall_arch.h" ]; then
    cp arch/x86_64/syscall_arch.h "$upstream_snapshot/syscall_arch.h"
fi
if [ ! -f "$upstream_snapshot/syscall_cp.s" ]; then
    cp src/thread/x86_64/syscall_cp.s "$upstream_snapshot/syscall_cp.s"
fi
if [ ! -f "$upstream_snapshot/clone.s" ]; then
    cp src/thread/x86_64/clone.s "$upstream_snapshot/clone.s"
fi
if [ ! -f "$upstream_snapshot/restore.s" ]; then
    cp src/signal/x86_64/restore.s "$upstream_snapshot/restore.s"
fi
if [ ! -f "$upstream_snapshot/vfork.s" ]; then
    cp src/process/x86_64/vfork.s "$upstream_snapshot/vfork.s"
fi
if [ ! -f "$upstream_snapshot/__set_thread_area.s" ]; then
    cp src/thread/x86_64/__set_thread_area.s \
        "$upstream_snapshot/__set_thread_area.s"
fi
if [ ! -f "$upstream_snapshot/__unmapself.s" ]; then
    cp src/thread/x86_64/__unmapself.s "$upstream_snapshot/__unmapself.s"
fi

config_include_dir=${PEDIGREE_CONFIG_INCLUDE_DIR:-$SRCDIR/build}
rm -f src/internal/pedigree-musl.c
case "$ARCH_TARGET" in
    HOSTED)
        pedigree_cppflags="-I$SRCDIR/src/modules/subsys/posix/syscalls"
        pedigree_cppflags="$pedigree_cppflags -I$SRCDIR/src/system/include"
        pedigree_cppflags="$pedigree_cppflags -I$config_include_dir -DHOSTED=1"
        cp "$SRCDIR/src/modules/subsys/posix/musl/glue-musl.c" \
            src/internal/pedigree-musl.c
        cp "$SRCDIR/src/modules/subsys/posix/musl/clone-hosted-amd64.musl-s" \
            src/thread/x86_64/clone.s
        cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_arch.h" \
            arch/x86_64/syscall_arch.h
        cp "$SRCDIR/src/modules/subsys/posix/musl/syscall_cp-amd64.musl-s" \
            src/thread/x86_64/syscall_cp.s
        # These upstream trampolines issue raw Linux syscalls. Hosted builds
        # select musl's C fallbacks so every call stays behind the bridge.
        rm -f src/signal/x86_64/restore.s
        rm -f src/process/x86_64/vfork.s
        rm -f src/thread/x86_64/{__unmapself,__set_thread_area}.s
        # Hosted page geometry is supplied through AT_PAGESZ at runtime.
        : >arch/x86_64/bits/limits.h
        ;;
    X64)
        pedigree_cppflags=
        cp "$upstream_snapshot/clone.s" src/thread/x86_64/clone.s
        cp "$upstream_snapshot/syscall_arch.h" arch/x86_64/syscall_arch.h
        cp "$upstream_snapshot/syscall_cp.s" src/thread/x86_64/syscall_cp.s
        cp "$upstream_snapshot/restore.s" src/signal/x86_64/restore.s
        cp "$upstream_snapshot/vfork.s" src/process/x86_64/vfork.s
        cp "$upstream_snapshot/__set_thread_area.s" \
            src/thread/x86_64/__set_thread_area.s
        cp "$upstream_snapshot/__unmapself.s" \
            src/thread/x86_64/__unmapself.s
        printf '#define PAGESIZE 4096\n' >arch/x86_64/bits/limits.h
        ;;
    *)
        echo "Unsupported amd64 musl architecture target: $ARCH_TARGET" >&2
        exit 1
        ;;
esac

rm -rf build
mkdir -p build
cd build

date >musl.log 2>&1

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
staged_target="$stage_root/root"

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

CPPFLAGS="$pedigree_cppflags" \
CFLAGS="-O2 -g3 -ggdb -fno-omit-frame-pointer -fPIC" CROSS_COMPILE="$cross_compile_prefix" \
LDFLAGS="$musl_ldflags" \
../configure --target=$COMPILER_TARGET --prefix=/usr \
    --syslibdir=/usr/lib --enable-shared \
    >>musl.log 2>&1 || die

make >>musl.log 2>&1 || die
make install DESTDIR="$staged_target" >>musl.log 2>&1 || die

loader="$staged_target/usr/lib/ld-musl-x86_64.so.1"
if [ ! -L "$loader" ]; then
    echo "The staged musl loader is not a symlink: $loader" >>musl.log
    die
fi
rm "$loader" >>musl.log 2>&1 || die
ln -s libc.so "$loader" >>musl.log 2>&1 || die

# Existing compiler installations can still point at build/musl/{include,lib}.
# Keep those projections until all consumers use the versioned SDK provider.
ln -s usr/include "$staged_target/include" >>musl.log 2>&1 || die
ln -s usr/lib "$staged_target/lib" >>musl.log 2>&1 || die

# Refuse to install a libc that silently selected the generic -ENOSYS fallback
# or the wrong target's trampoline.
clone_disassembly=$(
    "$objdump_tool" -d --disassemble=__clone \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
clone_syscalls=$(printf '%s\n' "$clone_disassembly" | grep -c '[[:space:]]syscall')
syscall_cp_disassembly=$(
    "$objdump_tool" -d --disassemble=__syscall_cp_asm \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
syscall_cp_syscalls=$(printf '%s\n' "$syscall_cp_disassembly" | grep -c '[[:space:]]syscall')
restore_disassembly=$(
    "$objdump_tool" -d --disassemble=__restore_rt \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
restore_syscalls=$(printf '%s\n' "$restore_disassembly" | grep -c '[[:space:]]syscall')
vfork_disassembly=$(
    "$objdump_tool" -d --disassemble=vfork \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
vfork_syscalls=$(printf '%s\n' "$vfork_disassembly" | grep -c '[[:space:]]syscall')
set_thread_area_disassembly=$(
    "$objdump_tool" -d --disassemble=__set_thread_area \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
set_thread_area_syscalls=$(
    printf '%s\n' "$set_thread_area_disassembly" | grep -c '[[:space:]]syscall'
)
unmapself_disassembly=$(
    "$objdump_tool" -d --disassemble=__unmapself \
        "$staged_target/usr/lib/libc.so" 2>>musl.log
) || die
unmapself_syscalls=$(
    printf '%s\n' "$unmapself_disassembly" | grep -c '[[:space:]]syscall'
)
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
        if [ "$syscall_cp_syscalls" -ne 0 ] || \
            ! printf '%s\n' "$syscall_cp_disassembly" | \
                grep -q 'pedigree_translate_syscall'; then
            echo "Hosted musl cancellation did not retain its syscall bridge." \
                >>musl.log
            printf '%s\n' "$syscall_cp_disassembly" >>musl.log
            die
        fi
        if [ "$restore_syscalls" -ne 0 ] || \
            [ "$vfork_syscalls" -ne 0 ] || \
            [ "$set_thread_area_syscalls" -ne 0 ] || \
            [ "$unmapself_syscalls" -ne 0 ]; then
            echo "Hosted musl process, thread, and signal trampolines contain raw syscalls." \
                >>musl.log
            printf '%s\n' "$restore_disassembly" >>musl.log
            printf '%s\n' "$vfork_disassembly" >>musl.log
            printf '%s\n' "$set_thread_area_disassembly" >>musl.log
            printf '%s\n' "$unmapself_disassembly" >>musl.log
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
        if [ "$syscall_cp_syscalls" -lt 1 ] || \
            printf '%s\n' "$syscall_cp_disassembly" | \
                grep -q 'pedigree_translate_syscall'; then
            echo "Native musl cancellation did not use the raw Linux syscall ABI." \
                >>musl.log
            printf '%s\n' "$syscall_cp_disassembly" >>musl.log
            die
        fi
        if [ "$restore_syscalls" -lt 1 ] || \
            [ "$vfork_syscalls" -lt 1 ] || \
            [ "$set_thread_area_syscalls" -lt 1 ] || \
            [ "$unmapself_syscalls" -lt 2 ]; then
            echo "Native musl did not retain its upstream process, thread, and signal trampolines." \
                >>musl.log
            printf '%s\n' "$restore_disassembly" >>musl.log
            printf '%s\n' "$vfork_disassembly" >>musl.log
            printf '%s\n' "$set_thread_area_disassembly" >>musl.log
            printf '%s\n' "$unmapself_disassembly" >>musl.log
            die
        fi
        ;;
esac

if [ "$musl_dtrelr" -eq 1 ] && \
    ! "$readelf_tool" -d "$staged_target/usr/lib/libc.so" 2>>musl.log | \
        grep -q '(RELR)'; then
    echo "DT_RELR was requested but the staged libc does not contain it." >>musl.log
    die
fi

"$CMAKE_COMMAND" \
    "-DSDK_ROOT=$staged_target" \
    "-DOUTPUT=$staged_target/usr/share/pedigree/libc/manifest.json" \
    "-DMUSL_VERSION=$MUSL_VERSION" \
    "-DPORT_REVISION=$PEDIGREE_MUSL_PORT_REVISION" \
    -DARCHITECTURE=x86_64 \
    "-DABI=$PEDIGREE_MUSL_ABI" \
    -DLAYOUT=fhs-usr-v1 \
    "-DPROFILE=$PEDIGREE_MUSL_PROFILE" \
    "-DPAGE_SIZE=$PEDIGREE_TARGET_PAGE_SIZE" \
    "-DDT_RELR=$PEDIGREE_DTRELR" \
    "-DUPSTREAM_SHA256=$MUSL_UPSTREAM_SHA256" \
    "-DPEDIGREE_REVISION=$PEDIGREE_MUSL_REVISION" \
    "-DBUILD_ID=$PEDIGREE_MUSL_BUILD_ID" \
    "-DCOMPILER_TARGET=$PEDIGREE_MUSL_COMPILER_TARGET" \
    "-DCOMPILER_VERSION=$PEDIGREE_MUSL_COMPILER_VERSION" \
    -P "$PEDIGREE_MUSL_MANIFEST_GENERATOR" >>musl.log 2>&1 || die

# Replace the complete SDK only after validating it. Replacing the tree,
# instead of installing over it, also removes files left by older builds.
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
