#!/usr/bin/env bash

# Build the native utilities and the hosted Pedigree kernel.

set -e

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" != "1" ] &&
    [ "${PEDIGREE_HOSTED_NATIVE:-0}" != "1" ]; then
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

    docker_run_args=(
        --rm
        --init
        --platform linux/amd64
        --user "$(id -u):$(id -g)"
        --env HOME=/tmp
        --env PEDIGREE_HOSTED_CONTAINER=1
        --volume "$script_dir:$script_dir"
        --volume "$linux_build_dir/toolchain:$script_dir/pedigree-compiler"
        --workdir "$script_dir"
    )
    if [ -n "${PEDIGREE_VERIFY_LOG_DIR:-}" ]; then
        mkdir -p "$PEDIGREE_VERIFY_LOG_DIR"
        verify_log_dir=$(cd -P -- "$PEDIGREE_VERIFY_LOG_DIR" && pwd -P)
        docker_run_args+=(--env "PEDIGREE_VERIFY_LOG_DIR=$verify_log_dir")
        case "$verify_log_dir/" in
            "$script_dir/"*) ;;
            *) docker_run_args+=(--volume "$verify_log_dir:$verify_log_dir") ;;
        esac
    fi

    echo "Building the x86_64 Linux hosted kernel in Docker."
    docker build --platform linux/amd64 \
        --tag pedigree-hosted-build \
        --file "$script_dir/build-etc/docker/hosted.Dockerfile" \
        "$script_dir/build-etc/docker"
    docker run "${docker_run_args[@]}" \
        pedigree-hosted-build \
        ./easy_build_hosted.sh
    exit
fi

if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" != "1" ] &&
    { [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; }; then
    echo "PEDIGREE_HOSTED_NATIVE=1 requires x86-64 Linux." >&2
    exit 1
fi

COMPILER_DIR="$script_dir/pedigree-compiler"

if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" = "1" ]; then
    # The container image declares the hosted build dependencies. Running the
    # interactive host installer here would require sudo from an unprivileged
    # container user and would make clean builds depend on .easy_os.
    real_os=ubuntu
    host_build_dir="$script_dir/build-host-linux"
    hosted_build_dir="$script_dir/build-hosted-smoke-linux"
    dynamic_build_dir="$script_dir/build-hosted-modules-linux"
else
    real_os=ubuntu
    host_build_dir="$script_dir/build-host"
    hosted_build_dir="$script_dir/build-hosted-smoke"
    dynamic_build_dir="$script_dir/build-hosted-modules"
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

cmake_options=(
    -DPEDIGREE_WARNINGS=ON
    -DPEDIGREE_POSIX_VERBOSE=OFF
)

echo
echo "Configuring and building native utilities and tests."
cmake -S "$script_dir" -B "$host_build_dir" \
    -DPEDIGREE_BUILDUTILS_ASAN=OFF "${cmake_options[@]}"
cmake --build "$host_build_dir" --parallel \
    --target testsuite headerify ext2img keymap
ctest --test-dir "$host_build_dir" --output-on-failure --no-tests=error

echo
echo "Configuring and building the static hosted smoke kernel and disk."
cmake -S "$script_dir" -B "$hosted_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted.cmake" \
    -DIMPORT_EXECUTABLES="$host_build_dir/HostUtilities.cmake" \
    -DPEDIGREE_HOSTED_ASAN=OFF \
    -DPEDIGREE_HOSTED_DYNAMIC_MODULES=OFF \
    -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
    -DPEDIGREE_WITH_INIT=ON \
    "${cmake_options[@]}"
# musl invokes its own make; serialising this prerequisite avoids leaking
# CMake's parallel jobserver file descriptors into the nested build.
cmake --build "$hosted_build_dir" --parallel 1 --target libc
cmake --build "$hosted_build_dir" --parallel --target kernelfinal hddimage

echo
echo "Configuring and building the dynamic hosted module smoke kernel."
cmake -S "$script_dir" -B "$dynamic_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted.cmake" \
    -DIMPORT_EXECUTABLES="$host_build_dir/HostUtilities.cmake" \
    -DPEDIGREE_HOSTED_ASAN=OFF \
    -DPEDIGREE_HOSTED_DYNAMIC_MODULES=ON \
    -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
    -DPEDIGREE_WITH_INIT=OFF \
    "${cmake_options[@]}"
cmake --build "$dynamic_build_dir" --parallel 1 --target libc
cmake --build "$dynamic_build_dir" --parallel \
    --target kernelfinal config hosted-smoke

"$script_dir/scripts/test-hosted-kernel.sh" \
    --static-kernel "$hosted_build_dir/src/system/kernel/kernel" \
    --dynamic-kernel "$dynamic_build_dir/src/system/kernel/kernel" \
    --dynamic-config-module "$dynamic_build_dir/src/modules/config.o" \
    --dynamic-smoke-module "$dynamic_build_dir/src/modules/hosted-smoke.o" \
    --config "$hosted_build_dir/config.db" \
    --disk-image "$hosted_build_dir/hdd.img"

cd "$old"

echo
echo "The hosted kernel and native utilities are ready."
if [ "${PEDIGREE_HOSTED_CONTAINER:-0}" = "1" ]; then
    echo "Re-run the complete build and smoke ladder from the host with:"
    echo "  ./easy_build_hosted.sh"
    exit
fi

echo "Rebuild the utilities with:"
echo "  cmake --build '$host_build_dir' --parallel --target testsuite headerify ext2img keymap"
echo "Rebuild the static hosted smoke artifacts with:"
echo "  cmake --build '$hosted_build_dir' --parallel 1 --target libc"
echo "  cmake --build '$hosted_build_dir' --parallel --target kernelfinal hddimage"
echo "Rebuild the dynamic hosted module smoke artifacts with:"
echo "  cmake --build '$dynamic_build_dir' --parallel 1 --target libc"
echo "  cmake --build '$dynamic_build_dir' --parallel --target kernelfinal config hosted-smoke"
echo "Re-run the hosted smoke ladder with:"
echo "  '$script_dir/scripts/test-hosted-kernel.sh' --static-kernel '$hosted_build_dir/src/system/kernel/kernel' --dynamic-kernel '$dynamic_build_dir/src/system/kernel/kernel' --dynamic-config-module '$dynamic_build_dir/src/modules/config.o' --dynamic-smoke-module '$dynamic_build_dir/src/modules/hosted-smoke.o' --config '$hosted_build_dir/config.db' --disk-image '$hosted_build_dir/hdd.img'"
