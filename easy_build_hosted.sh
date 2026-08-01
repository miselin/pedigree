#!/usr/bin/env bash

# Build the native utilities and the hosted Pedigree kernel.

set -e

old=$(pwd)
script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

hosted_parallel_args=(--parallel)
if [ -n "${PEDIGREE_VERIFY_JOBS:-}" ]; then
    if [[ ! "$PEDIGREE_VERIFY_JOBS" =~ ^[1-9][0-9]*$ ]]; then
        echo "PEDIGREE_VERIFY_JOBS must be a positive integer." >&2
        exit 2
    fi
    hosted_parallel_args=(--parallel "$PEDIGREE_VERIFY_JOBS")
fi

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
    if ! command -v python3 >/dev/null 2>&1; then
        echo "Python 3 is required to enforce the Docker build deadline." >&2
        exit 1
    fi

    docker_build_timeout=${PEDIGREE_HOSTED_DOCKER_BUILD_TIMEOUT_SECONDS:-1200}
    if [[ ! "$docker_build_timeout" =~ ^[1-9][0-9]*$ ]]; then
        echo "PEDIGREE_HOSTED_DOCKER_BUILD_TIMEOUT_SECONDS must be a positive integer." >&2
        exit 2
    fi
    reuse_image=${PEDIGREE_HOSTED_REUSE_IMAGE:-0}
    if [ "$reuse_image" != "0" ] && [ "$reuse_image" != "1" ]; then
        echo "PEDIGREE_HOSTED_REUSE_IMAGE must be 0 or 1." >&2
        exit 2
    fi
    hosted_image=${PEDIGREE_HOSTED_IMAGE:-pedigree-hosted-build}

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
    if [ -n "${PEDIGREE_VERIFY_JOBS:-}" ]; then
        docker_run_args+=(--env "PEDIGREE_VERIFY_JOBS=$PEDIGREE_VERIFY_JOBS")
    fi

    if [ "$reuse_image" = "1" ]; then
        image_id=$(
            docker image inspect --format '{{.Id}}' "$hosted_image" \
                2>/dev/null || true
        )
        if [ -z "$image_id" ]; then
            # Some Docker Desktop content-store versions list a tagged image
            # but fail name-based inspect/run lookup. Resolve the immutable ID
            # from the filtered image inventory in that case.
            image_id=$(
                docker image ls --no-trunc \
                    --format '{{.Repository}}:{{.Tag}} {{.ID}}' \
                    "$hosted_image" | awk 'NR == 1 { print $2 }'
            )
        fi
        if [ -z "$image_id" ]; then
            echo "The requested hosted build image is unavailable: $hosted_image" >&2
            exit 1
        fi
        echo "Reusing hosted build image $hosted_image ($image_id)."
        hosted_image=$image_id
    else
        echo "Building the x86_64 Linux hosted kernel in Docker."
        python3 "$script_dir/scripts/run-with-deadline.py" \
            --seconds "$docker_build_timeout" \
            --label "hosted Docker image build" -- \
            docker build --platform linux/amd64 \
                --tag "$hosted_image" \
                --file "$script_dir/build-etc/docker/hosted.Dockerfile" \
                "$script_dir/build-etc/docker"
    fi
    docker run "${docker_run_args[@]}" \
        "$hosted_image" \
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
    x64_build_dir="$script_dir/build-x64-check-linux"
else
    real_os=ubuntu
    host_build_dir="$script_dir/build-host"
    hosted_build_dir="$script_dir/build-hosted-smoke"
    dynamic_build_dir="$script_dir/build-hosted-modules"
    x64_build_dir="$script_dir/build-x64-check"
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

configure_hosted_build()
{
    local build_dir=$1
    local dynamic_modules=$2
    local with_init=$3
    local system_malloc=$4

    cmake -S "$script_dir" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted.cmake" \
        -DIMPORT_EXECUTABLES="$host_build_dir/HostUtilities.cmake" \
        -DPEDIGREE_HOSTED_ASAN=ON \
        -DPEDIGREE_HOSTED_SYSTEM_MALLOC="$system_malloc" \
        -DPEDIGREE_HOSTED_DYNAMIC_MODULES="$dynamic_modules" \
        -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
        -DPEDIGREE_WITH_INIT="$with_init" \
        "${cmake_options[@]}"
}

run_hosted_smoke()
{
    local heap=$1
    local log_command=()
    if [ -n "${PEDIGREE_VERIFY_LOG_DIR:-}" ]; then
        local heap_log_dir="$PEDIGREE_VERIFY_LOG_DIR/$heap"
        mkdir -p "$heap_log_dir"
        log_command=(env "PEDIGREE_VERIFY_LOG_DIR=$heap_log_dir")
    fi

    "${log_command[@]}" "$script_dir/scripts/test-hosted-kernel.sh" \
        --static-kernel "$hosted_build_dir/src/system/kernel/kernel" \
        --dynamic-kernel "$dynamic_build_dir/src/system/kernel/kernel" \
        --dynamic-config-module "$dynamic_build_dir/src/modules/config.o" \
        --dynamic-smoke-module "$dynamic_build_dir/src/modules/hosted-smoke.o" \
        --config "$hosted_build_dir/config.db" \
        --disk-image "$hosted_build_dir/hdd.img" \
        --require-asan \
        --expected-heap "$heap"
}

echo
echo "Configuring and building native utilities and tests."
cmake -S "$script_dir" -B "$host_build_dir" \
    -DPEDIGREE_BUILDUTILS_ASAN=OFF "${cmake_options[@]}"
cmake --build "$host_build_dir" "${hosted_parallel_args[@]}" \
    --target testsuite headerify ext2img keymap memorytracer unixsockets
ctest --test-dir "$host_build_dir" --output-on-failure --no-tests=error

first_heap=1
for heap in system slam; do
    if [ "$heap" = "system" ]; then
        system_malloc=ON
        heap_description="system malloc"
    else
        system_malloc=OFF
        heap_description="Pedigree SlamAllocator"
    fi

    echo
    echo "Configuring AddressSanitizer hosted kernels with $heap_description."
    configure_hosted_build "$hosted_build_dir" OFF ON "$system_malloc"
    configure_hosted_build "$dynamic_build_dir" ON OFF "$system_malloc"

    if [ "$first_heap" = "1" ]; then
        # musl invokes its own make; serialising this prerequisite avoids
        # leaking CMake's parallel jobserver file descriptors into the nested
        # build. The heap toggle does not change the userspace ABI.
        cmake --build "$hosted_build_dir" --parallel 1 --target libc
        cmake --build "$dynamic_build_dir" --parallel 1 --target libc
        first_heap=0
    fi

    cmake --build "$hosted_build_dir" "${hosted_parallel_args[@]}" \
        --target kernelfinal hddimage
    cmake --build "$dynamic_build_dir" "${hosted_parallel_args[@]}" \
        --target kernelfinal config users vfs fat rawfs usb hosted-smoke
    run_hosted_smoke "$heap"
done

echo
echo "Configuring the x86-64 PC compile-and-link matrix."
cmake -S "$script_dir" -B "$x64_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_amd64.cmake" \
    -DIMPORT_EXECUTABLES="$host_build_dir/HostUtilities.cmake" \
    -DPEDIGREE_WARNINGS=ON \
    -DPEDIGREE_POSIX_VERBOSE=OFF \
    -DPEDIGREE_STATIC_DRIVERS=OFF \
    -DPEDIGREE_WITH_INIT=ON

# musl invokes its own make and supplies the headers and CRT objects required
# by both the kernel's userspace boundary and every initrd module.
cmake --build "$x64_build_dir" --parallel 1 --target libc
cmake --build "$x64_build_dir" "${hosted_parallel_args[@]}" \
    --target kernelfinal initrd

for artifact in \
    "$x64_build_dir/src/system/kernel/kernel-mini64" \
    "$x64_build_dir/src/modules/initrd.tar"
do
    if [ ! -s "$artifact" ]; then
        echo "The x86-64 verification artifact is missing or empty: $artifact" >&2
        exit 1
    fi
done

cd "$old"

echo
echo "The AddressSanitizer hosted matrix, native utilities, and x86-64 PC artifacts are ready."
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
echo "  cmake --build '$dynamic_build_dir' --parallel --target kernelfinal config users vfs fat rawfs usb hosted-smoke"
echo "Re-run the hosted smoke ladder with:"
echo "  '$script_dir/scripts/test-hosted-kernel.sh' --static-kernel '$hosted_build_dir/src/system/kernel/kernel' --dynamic-kernel '$dynamic_build_dir/src/system/kernel/kernel' --dynamic-config-module '$dynamic_build_dir/src/modules/config.o' --dynamic-smoke-module '$dynamic_build_dir/src/modules/hosted-smoke.o' --config '$hosted_build_dir/config.db' --disk-image '$hosted_build_dir/hdd.img' --require-asan --expected-heap slam"
echo "Rebuild the x86-64 PC compile-and-link matrix with:"
echo "  cmake --build '$x64_build_dir' --parallel 1 --target libc"
echo "  cmake --build '$x64_build_dir' --parallel --target kernelfinal initrd"
