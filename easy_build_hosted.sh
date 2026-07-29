#!/usr/bin/env bash

# Build the native utilities and the hosted Pedigree kernel.

set -e

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

COMPILER_DIR="$script_dir/pedigree-compiler"
. "$script_dir/build-etc/travis.sh"
. "$script_dir/scripts/easy_build_deps.sh" "$@"

echo
echo "Checking for the x86_64 Pedigree toolchain."

compiler_build_options=()
if [ "$real_os" = "osx" ]; then
    compiler_build_options+=("osx-compat")
fi
compiler_build_options+=("sysroot=$script_dir/build-hosted/musl")

"$script_dir/scripts/checkBuildSystemNoInteractive.pl" \
    x86_64-pedigree "$COMPILER_DIR" "${compiler_build_options[@]}"

echo
echo "Initialising submodules."
git submodule update --init --recursive

cmake_options=(-DPEDIGREE_WARNINGS=ON)
if [ -n "$TRAVIS_OPTIONS" ]; then
    cmake_options+=("$TRAVIS_OPTIONS")
fi

echo
echo "Configuring and building native utilities and tests."
cmake -S "$script_dir" -B "$script_dir/build-host" "${cmake_options[@]}"
cmake --build "$script_dir/build-host" --parallel \
    --target testsuite headerify ext2img keymap

echo
echo "Configuring and building the hosted kernel."
cmake -S "$script_dir" -B "$script_dir/build-hosted" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted.cmake" \
    -DIMPORT_EXECUTABLES="$script_dir/build-host/HostUtilities.cmake" \
    "${cmake_options[@]}"
cmake --build "$script_dir/build-hosted" --parallel --target kernelfinal

cd "$old"

echo
echo "The hosted kernel and native utilities are ready."
echo "Rebuild the utilities with:"
echo "  cmake --build '$script_dir/build-host' --parallel --target testsuite headerify ext2img keymap"
echo "Rebuild the hosted kernel with:"
echo "  cmake --build '$script_dir/build-hosted' --parallel --target kernelfinal"
echo "Run the native GoogleTest suite with:"
echo "  ctest --test-dir '$script_dir/build-host' --output-on-failure"
