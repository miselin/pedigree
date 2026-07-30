#!/usr/bin/env bash

set -Eeuo pipefail

go_version=1.26.5
go_source_sha256=495be4bc87176ac567392e5b4116abd98466d33d7b49d41e764ccc6976b2dc42

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)
build_root=${PEDIGREE_GO_BUILD_DIR:-"$repo_root/build-go-pedigree"}
archive="$build_root/go${go_version}.src.tar.gz"
go_root="$build_root/go"
build_cache="$build_root/cache"
patch_file="$repo_root/compilers/go${go_version}-pedigree.patch"
patch_marker="$go_root/.pedigree-patched"

if [ -z "${GOROOT_BOOTSTRAP:-}" ]; then
    if command -v go >/dev/null 2>&1; then
        GOROOT_BOOTSTRAP=$(go env GOROOT)
    else
        echo "GOROOT_BOOTSTRAP must point to Go 1.24.6 or newer." >&2
        exit 1
    fi
fi

mkdir -p "$build_root" "$build_cache"

if [ ! -f "$archive" ]; then
    curl -fL --retry 3 \
        -o "$archive" \
        "https://go.dev/dl/go${go_version}.src.tar.gz"
fi

if command -v sha256sum >/dev/null 2>&1; then
    archive_sha256=$(sha256sum "$archive" | awk '{print $1}')
else
    archive_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
fi

if [ "$archive_sha256" != "$go_source_sha256" ]; then
    echo "Go source checksum mismatch: $archive_sha256" >&2
    exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
    patch_sha256=$(sha256sum "$patch_file" | awk '{print $1}')
else
    patch_sha256=$(shasum -a 256 "$patch_file" | awk '{print $1}')
fi

if [ -f "$patch_marker" ] &&
    [ "$(cat "$patch_marker")" != "$patch_sha256" ]; then
    echo "Pedigree patch changed; refreshing the Go source tree."
    rm -rf -- "$go_root"
fi

if [ ! -d "$go_root" ]; then
    tar -xzf "$archive" -C "$build_root"
fi

if [ ! -f "$patch_marker" ]; then
    patch -d "$go_root" -p1 < "$patch_file"
    printf '%s\n' "$patch_sha256" > "$patch_marker"
fi

(
    cd "$go_root/src"
    env \
        GOROOT_BOOTSTRAP="$GOROOT_BOOTSTRAP" \
        GOCACHE="$build_cache" \
        GOENV=off \
        ./make.bash
)

dist_targets=$("$go_root/bin/go" tool dist list)
if ! grep -qx 'pedigree/amd64' <<< "$dist_targets"; then
    echo "Built toolchain does not advertise pedigree/amd64." >&2
    exit 1
fi

env \
    GOOS=pedigree \
    GOARCH=amd64 \
    CGO_ENABLED=0 \
    GOCACHE="$build_cache" \
    GOENV=off \
    "$go_root/bin/go" build \
    -trimpath \
    -o "$build_root/go-canary" \
    "$repo_root/src/user/applications/go-canary/main.go"

echo "Go toolchain: $go_root"
echo "Pedigree canary: $build_root/go-canary"
