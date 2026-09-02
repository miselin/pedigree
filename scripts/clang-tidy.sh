#!/usr/bin/env bash

set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: scripts/clang-tidy.sh host|cross|userspace <build-directory> [options]

The build directory must contain compile_commands.json and the LLVM 22
clang-tidy tools. Cross and userspace modes expect the x86_64 Pedigree toolchain
under PEDIGREE_TOOLCHAIN_ROOT (default: /opt/pedigree). Userspace also requires
the ignored images/local SDK and a build configured with
PEDIGREE_BUILD_USER_DIR=ON.
EOF
}

if (( $# < 2 )); then
    usage
    exit 2
fi

mode=$1
build_dir=$2
shift 2

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

if [[ $build_dir != /* ]]; then
    build_dir="$repo_root/$build_dir"
fi

compile_db="$build_dir/compile_commands.json"
if [[ ! -s $compile_db ]]; then
    echo "clang-tidy: missing compilation database: $compile_db" >&2
    exit 1
fi

for command_name in clang-tidy run-clang-tidy python3; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "clang-tidy: required command not found: $command_name" >&2
        exit 1
    fi
done

tidy_version=$(clang-tidy --version)
if [[ $tidy_version != *"version "* ]]; then
    echo "clang-tidy: unable to determine clang-tidy version" >&2
    exit 1
fi
tidy_major=${tidy_version#*version }
tidy_major=${tidy_major%%.*}
if [[ ! $tidy_major =~ ^[0-9]+$ ]] || (( tidy_major != 22 )); then
    echo "clang-tidy: LLVM 22 is required" >&2
    exit 1
fi

runner_help=$(run-clang-tidy --help)
for required_option in -source-filter -removed-arg; do
    if [[ $runner_help != *"$required_option"* ]]; then
        echo "clang-tidy: run-clang-tidy does not support $required_option" >&2
        exit 1
    fi
done

escaped_repo_root=$(python3 -c \
    'import re, sys; print(re.escape(sys.argv[1]))' "$repo_root")

tidy_args=(-p "$build_dir")
case $mode in
    host)
        # LLVM's analyzer does not model the atomic reference-count increment
        # in one overlapping SharedPointer copy test. That test has its own TU,
        # analyzed in a second pass with only NewDelete disabled.
        host_atomic_filter="^${escaped_repo_root}/src/buildutil/testsuite/test-SharedPointerCopy\\.cc$"
        source_filter="^${escaped_repo_root}/src/buildutil/(?!testsuite/test-SharedPointerCopy\\.cc$).*\\.(c|cc|cpp|cxx)$"
        required_sources=(
            "$repo_root/src/buildutil/testsuite/test-String.cc"
            "$repo_root/src/buildutil/testsuite/test-utility.cc"
            "$repo_root/src/buildutil/testsuite/test-SharedPointerCopy.cc"
        )
        ;;
    cross)
        # Keep Pedigree's lwIP integration, CDI shims, and POSIX glue in scope.
        # The excluded directories are imported sources.
        source_filter="^${escaped_repo_root}/src/(system/kernel/(?!debugger/libudis86(/|$)|machine/mach_pc/x86emu(/|$)|utilities/(md5|sha1|smhasher|spooky)(/|$))|modules/(?!system/config/sqlite3(/|$)|system/lwip/(api|core|netif)(/|$))).*\\.(c|cc|cpp|cxx)$"
        required_sources=(
            "$repo_root/src/modules/drivers/common/cdi/CdiCmos.cc"
            "$repo_root/src/modules/drivers/common/cdi/CdiIrq.cc"
            "$repo_root/src/modules/subsys/posix/posix.cc"
            "$repo_root/src/modules/system/lwip/lwip.cc"
            "$repo_root/src/modules/system/lwip/sys_arch.cc"
        )
        ;;
    userspace)
        # nyancat is an imported application. Generated keymap parser and
        # scanner sources are outside src/ and therefore do not match.
        source_filter="^${escaped_repo_root}/src/user/(?!applications/nyancat(/|$)).*\\.(c|cc|cpp|cxx)$"
        required_sources=(
            "$repo_root/src/user/applications/gears/gears.cc"
            "$repo_root/src/user/applications/init/main.c"
            "$repo_root/src/user/libraries/libui/src/protocol.cc"
        )

        required_sdk_headers=(
            "$repo_root/images/local/include/GL/osmesa.h"
            "$repo_root/images/local/include/cairo/cairo.h"
            "$repo_root/images/local/include/pango-1.0/pango/pango.h"
            "$repo_root/images/local/libraries/glib-2.0/include/glibconfig.h"
        )
        for sdk_header in "${required_sdk_headers[@]}"; do
            if [[ ! -f $sdk_header ]]; then
                echo "clang-tidy: userspace SDK header missing: $sdk_header" >&2
                echo "clang-tidy: provision images/local before running userspace tidy" >&2
                exit 1
            fi
        done
        ;;
    *)
        usage
        exit 2
        ;;
esac

if [[ $mode != host ]]; then
    toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-/opt/pedigree}
    compiler_target=${PEDIGREE_COMPILER_TARGET:-x86_64-pedigree}
    cross_cxx=${PEDIGREE_CXX_COMPILER:-${compiler_target}-g++}
    if ! command -v "$cross_cxx" >/dev/null 2>&1; then
        echo "clang-tidy: cross compiler not found: $cross_cxx" >&2
        exit 1
    fi

    gcc_version=$("$cross_cxx" -dumpversion)
    include_dirs=(
        "$toolchain_root/include/c++/$gcc_version"
        "$toolchain_root/include/c++/$gcc_version/$compiler_target"
        "$toolchain_root/include/c++/$gcc_version/backward"
        "$toolchain_root/lib/gcc/$compiler_target/$gcc_version/include"
        "$toolchain_root/lib/gcc/$compiler_target/$gcc_version/include-fixed"
        "$toolchain_root/$compiler_target/include"
    )
    for include_dir in "${include_dirs[@]}"; do
        if [[ ! -d $include_dir ]]; then
            echo "clang-tidy: missing cross include directory: $include_dir" >&2
            exit 1
        fi
        tidy_args+=("-extra-arg-before=-isystem")
        tidy_args+=("-extra-arg-before=$include_dir")
    done

    tidy_args+=(
        -removed-arg=-march=k8
        -extra-arg-before=-nostdinc++
        "-extra-arg-before=--target=$compiler_target"
        -extra-arg-before=-D_GNU_SOURCE=1
    )
fi

# run-clang-tidy succeeds when its source filter matches nothing. Check both
# the filter and representative coverage anchors before starting the run.
python3 - "$compile_db" "$source_filter" "${required_sources[@]}" <<'PY'
import json
import re
import sys

compile_db, source_filter, *required_sources = sys.argv[1:]
with open(compile_db, encoding="utf-8") as handle:
    commands = json.load(handle)

compiled_sources = {entry["file"] for entry in commands}
missing_sources = [path for path in required_sources if path not in compiled_sources]
if missing_sources:
    print("clang-tidy: compilation database is missing required sources:", file=sys.stderr)
    for path in missing_sources:
        print(f"  {path}", file=sys.stderr)
    sys.exit(1)

pattern = re.compile(source_filter)
selected_sources = sorted(path for path in compiled_sources if pattern.match(path))
if not selected_sources:
    print("clang-tidy: source filter selected no compilation units", file=sys.stderr)
    sys.exit(1)

print(f"clang-tidy: selected {len(selected_sources)} compilation units")
PY

if [[ $mode == host ]]; then
    run-clang-tidy \
        "${tidy_args[@]}" \
        -source-filter="$source_filter" \
        "$@"

    exec run-clang-tidy \
        "${tidy_args[@]}" \
        -source-filter="$host_atomic_filter" \
        -checks=-clang-analyzer-cplusplus.NewDelete \
        "$@"
else
    exec run-clang-tidy \
        "${tidy_args[@]}" \
        -source-filter="$source_filter" \
        "$@"
fi
