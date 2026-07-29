#!/usr/bin/env bash

# Build the native utilities and the hosted Pedigree kernel.

set -e

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" != "1" ] &&
    { [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; }; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "A running Docker installation is required for the x86_64 Linux hosted kernel." >&2
        exit 1
    fi
    if ! docker info >/dev/null 2>&1; then
        echo "Docker is installed but is not running." >&2
        exit 1
    fi

    linux_build_dir="$script_dir/build-hosted-linux"
    mkdir -p "$linux_build_dir/toolchain"

    echo "Building the x86_64 Linux hosted kernel in Docker."
    docker build --platform linux/amd64 \
        --tag pedigree-hosted-build \
        --file "$script_dir/build-etc/docker/hosted.Dockerfile" \
        "$script_dir/build-etc/docker"
    docker run --rm --init --platform linux/amd64 \
        --user "$(id -u):$(id -g)" \
        --env HOME=/tmp \
        --env PEDIGREE_HOSTED_CONTAINER=1 \
        --volume "$script_dir:$script_dir" \
        --volume "$linux_build_dir/toolchain:$script_dir/pedigree-compiler" \
        --workdir "$script_dir" \
        pedigree-hosted-build \
        ./easy_build_hosted.sh
    exit
fi

COMPILER_DIR="$script_dir/pedigree-compiler"
. "$script_dir/build-etc/travis.sh"
. "$script_dir/scripts/easy_build_deps.sh" "$@"

if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" = "1" ]; then
    real_os=ubuntu
    host_build_dir="$script_dir/build-host-linux"
    hosted_build_dir="$script_dir/build-hosted-linux"
else
    host_build_dir="$script_dir/build-host"
    hosted_build_dir="$script_dir/build-hosted"
fi

echo
echo "Checking for the x86_64 Pedigree toolchain."

compiler_build_options=()
if [ "$real_os" = "osx" ]; then
    compiler_build_options+=("osx-compat")
fi
compiler_build_options+=("sysroot=$hosted_build_dir/musl")

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
cmake -S "$script_dir" -B "$host_build_dir" "${cmake_options[@]}"
cmake --build "$host_build_dir" --parallel \
    --target testsuite headerify ext2img keymap

echo
echo "Configuring and building the hosted kernel."
cmake -S "$script_dir" -B "$hosted_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted.cmake" \
    -DIMPORT_EXECUTABLES="$host_build_dir/HostUtilities.cmake" \
    "${cmake_options[@]}"
cmake --build "$hosted_build_dir" --parallel --target kernelfinal

cd "$old"

echo
echo "The hosted kernel and native utilities are ready."
echo "Rebuild the utilities with:"
echo "  cmake --build '$host_build_dir' --parallel --target testsuite headerify ext2img keymap"
echo "Rebuild the hosted kernel with:"
echo "  cmake --build '$hosted_build_dir' --parallel --target kernelfinal"
echo "Run the native GoogleTest suite with:"
echo "  ctest --test-dir '$host_build_dir' --output-on-failure"
