#!/bin/bash

set -e

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)

ALPINE_ARCH=${1:-x86_64}
case "$ALPINE_ARCH" in
    x86_64)
        output_dir="$script_dir/build"
        docker_platform=linux/amd64
        ;;
    aarch64)
        output_dir="$script_dir/build/aarch64"
        docker_platform=linux/arm64
        if [[ $(uname -s) == Darwin && $(uname -m) == arm64 ]]; then
            exec "$script_dir/build-native-aarch64.sh" "$output_dir"
        fi
        ;;
    *)
        echo "Usage: $0 [x86_64|aarch64]" >&2
        exit 1
        ;;
esac

cd "$script_dir"

mkdir -p "$output_dir"

docker build --platform "$docker_platform" -t "pedigree-alpine-rootfs:$ALPINE_ARCH" .
exec docker run --rm --platform "$docker_platform" \
    -e "ALPINE_ARCH=$ALPINE_ARCH" \
    -v "$output_dir:/out" \
    "pedigree-alpine-rootfs:$ALPINE_ARCH"
