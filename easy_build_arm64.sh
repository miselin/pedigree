#!/bin/bash

# Script that can be run to set up a Pedigree repository for building with minimal
# effort.

# Historical full-system bootstrap helper. The maintained verification path is
# ./verify.sh; see RESTORATION.md before relying on this script.

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P) && script_dir=$script_dir
cd $old

COMPILER_DIR=${PEDIGREE_TOOLCHAIN_ROOT:-$script_dir/pedigree-compiler-15.3.0-r2}
case $COMPILER_DIR in
    /*) ;;
    *) COMPILER_DIR=$old/$COMPILER_DIR ;;
esac

set -e

[ -d ".venv" ] || uv venv

. $script_dir/scripts/easy_build_deps.sh

echo "Please wait, checking for a working cross-compiler."
echo "If none is found, the source code for one will be downloaded, and it will be"
echo "compiled for you."

# Install cross-compilers
python3 "$script_dir/scripts/bootstrap_toolchain.py" \
    arm64-elf "$COMPILER_DIR" \
    --source-root "$script_dir"
COMPILER_DIR=$(cd -P -- "$COMPILER_DIR" && pwd -P)

old=$(pwd)

# Fix up POSIX headers which sometimes get a recursive symlink.
rm -f src/modules/subsys/posix/include/include || true

set +e

# Update the local working copy only if it is clean.
changed=`git status -s -uno`
if [ -z "$changed" ]; then
    git pull --rebase > /dev/null 2>&1
fi

if [ -d "src/modules/drivers/cdi" ]; then
    cd src/modules/drivers/cdi
    git pull || echo "Failed to update cdi."
    cd ${old}
else
    git clone git://git.tyndur.org/cdi.git src/modules/drivers/cdi || echo "Failed to clone cdi, cdi will not be part of your build."
fi

set -e

refresh_cmake_metadata=false
if [ -f build-arm64/CMakeCache.txt ]; then
    if ! grep -Fqx \
        "PEDIGREE_TOOLCHAIN_ROOT:PATH=$COMPILER_DIR" build-arm64/CMakeCache.txt; then
        refresh_cmake_metadata=true
    elif ! grep -Fqs 'set(CMAKE_SYSTEM_NAME "Pedigree")' \
        build-arm64/CMakeFiles/*/CMakeSystem.cmake 2>/dev/null; then
        refresh_cmake_metadata=true
    fi
fi

if [ "$refresh_cmake_metadata" = true ]; then
    # Compiler and target-platform identities are immutable CMake cache facts.
    # Preserve build outputs, but regenerate that metadata when either changes.
    cmake -E rm -f build-arm64/CMakeCache.txt
    cmake -E remove_directory build-arm64/CMakeFiles
fi
cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=${script_dir}/build-etc/cmake/pedigree_arm64.cmake \
    -DPEDIGREE_TOOLCHAIN_ROOT="$COMPILER_DIR"

echo "**** ARM64 Easy Build is a WORK IN PROGRESS ****"
echo "Stopping here, nothing else is implemented yet, have fun!"
