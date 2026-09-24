#!/bin/bash
set -euo pipefail

platform=$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')
image="pedigree-alpine-rootfs:3.22-${platform#linux/}"
if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "Prepare the Alpine SDK with scripts/alpine/build.sh before running protoc." >&2
    exit 1
fi

arguments=(--rm --platform "$platform" --entrypoint protoc --user "$(id -u):$(id -g)")
if [[ $# -ne 1 || $1 != --version ]]; then
    : "${PEDIGREE_WINMAN_SOURCE_DIR:?Set the window manager source directory}"
    : "${PEDIGREE_BUILD_DIR:?Set the CMake build directory}"
    source_dir=$(cd -P -- "$PEDIGREE_WINMAN_SOURCE_DIR" && pwd -P)
    build_dir=$(cd -P -- "$PEDIGREE_BUILD_DIR" && pwd -P)
    working_dir=$(pwd -P)
    case "$working_dir" in
        "$build_dir"|"$build_dir"/*|"$source_dir"|"$source_dir"/*) ;;
        *) echo "Run protoc from the source or build directory." >&2; exit 1 ;;
    esac
    arguments+=(-v "$source_dir:$source_dir:ro" -v "$build_dir:$build_dir:rw" -w "$working_dir")
fi
exec docker run "${arguments[@]}" "$image" "$@"
