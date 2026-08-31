#!/usr/bin/env bash

set -euo pipefail

files=$(git ls-files -- \
    .github/workflows/builder-image.yml \
    .dockerignore \
    build-etc/docker/pedigree-builder.Dockerfile \
    build-etc/cmake/pedigree_amd64.cmake \
    build-etc/toolchain \
    'compilers/pedigree-*.patch' \
    CMakeLists.txt \
    scripts/bootstrap_toolchain.py \
    scripts/build-musl-amd64.sh \
    scripts/ci/builder-fingerprint.sh \
    src/modules/CMakeLists.txt \
    src/modules/subsys/posix/musl \
    src/modules/subsys/posix/syscalls \
    src/system/include/pedigree/kernel/config.h.in \
    src/system/include/pedigree/kernel/processor/hosted/syscall-stubs.h \
    src/system/include/pedigree/kernel/processor/syscall-stubs.h \
    src/system/kernel/CMakeLists.txt | sort)
test -n "$files"

fingerprint=$(
    while IFS= read -r file; do
        printf '%s  %s\n' "$(shasum -a 256 "$file" | cut -d ' ' -f1)" "$file"
    done <<< "$files" | shasum -a 256 | cut -c1-16
)

printf '%s\n' "$fingerprint"
