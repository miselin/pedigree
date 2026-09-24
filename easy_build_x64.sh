#!/bin/bash

# Script that can be run to set up a Pedigree repository for building with minimal
# effort.

# Historical full-system bootstrap helper. The maintained verification path is
# ./verify.sh; see RESTORATION.md before relying on this script.

set -e

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
invoking_dir=$PWD
cd "$script_dir"

COMPILER_DIR=${PEDIGREE_TOOLCHAIN_ROOT:-$script_dir/pedigree-compiler-15.3.0-r2}
case $COMPILER_DIR in
    /*) ;;
    *) COMPILER_DIR=$invoking_dir/$COMPILER_DIR ;;
esac

[ -d ".venv" ] || uv venv

. "$script_dir/scripts/easy_build_deps.sh"

git submodule update --init

echo "Please wait, checking for a working cross-compiler."
echo "If none is found, the source code for one will be downloaded, and it will be"
echo "compiled for you."

# Install cross-compilers
uv run python "$script_dir/scripts/bootstrap_toolchain.py" \
    x86_64-pedigree "$COMPILER_DIR" \
    --source-root "$script_dir"
COMPILER_DIR=$(cd -P -- "$COMPILER_DIR" && pwd -P)

echo
echo "Configuring the Pedigree UPdater..."

uv run python "$script_dir/setup_pup.py" amd64
"$script_dir/run_pup.sh" sync

# Needed by the in-tree user applications
"$script_dir/run_pup.sh" install ncurses

refresh_cmake_metadata=false
if [ -f build/CMakeCache.txt ]; then
    if ! grep -Fqx \
        "PEDIGREE_TOOLCHAIN_ROOT:PATH=$COMPILER_DIR" build/CMakeCache.txt; then
        refresh_cmake_metadata=true
    elif ! grep -Fqs 'set(CMAKE_SYSTEM_NAME "Pedigree")' \
        build/CMakeFiles/*/CMakeSystem.cmake 2>/dev/null; then
        refresh_cmake_metadata=true
    fi
fi

if [ "$refresh_cmake_metadata" = true ]; then
    # Compiler and target-platform identities are immutable CMake cache facts.
    # Preserve build outputs, but regenerate that metadata when either changes.
    cmake -E rm -f build/CMakeCache.txt
    cmake -E remove_directory build/CMakeFiles
fi
cmake -S "$script_dir" -B "$script_dir/build" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_amd64.cmake" \
    -DPEDIGREE_TOOLCHAIN_ROOT="$COMPILER_DIR"

# CMake has acquired the packaged libc SDK needed to finish libstdc++.
uv run python "$script_dir/scripts/bootstrap_toolchain.py" \
    x86_64-pedigree "$COMPILER_DIR" \
    --source-root "$script_dir" --activate --libcpp

echo
echo "Installing a base set of packages..."

for package in pedigree-base libpng libfreetype libiconv zlib bash coreutils \
    fontconfig pixman cairo expat mesa gettext pango glib libpcre harfbuzz \
    libffi dialog protobuf gcc; do
    "$script_dir/run_pup.sh" install "$package"
done

echo
echo "Beginning the Pedigree build."
echo

cmake --build "$script_dir/build"

echo
echo
echo "Pedigree is now ready to be built without running this script."
echo "To build in future, run the following command in the '$script_dir' directory:"
echo "cmake --build build"
echo
echo "If you wish, you can continue to run this script. It won't ask questions"
echo "anymore, unless you remove the '.easy_os' file in '$script_dir'."
echo
echo "See README.md and RESTORATION.md for the maintained commands and current"
echo "support status."
echo
echo "Have fun with Pedigree! :)"
