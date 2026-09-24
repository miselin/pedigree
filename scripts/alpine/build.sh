#!/bin/bash
set -euo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
arch=${1:-x86_64}
if (( $# )); then shift; fi
profile=base
output_dir=
refresh=false

usage() {
    echo "Usage: $0 [x86_64|aarch64|armv7] [OUTPUT_DIR] [--profile base|desktop] [--refresh]" >&2
    exit 1
}
case "$arch" in x86_64|aarch64|armv7) ;; *) usage ;; esac
while (( $# )); do
    case "$1" in
        --profile)
            (( $# >= 2 )) || usage
            profile=$2
            shift 2
            ;;
        --refresh) refresh=true; shift ;;
        -*) usage ;;
        *)
            [[ -z $output_dir ]] || usage
            output_dir=$1
            shift
            ;;
    esac
done
case "$profile" in base|desktop) ;; *) usage ;; esac
output_dir=${output_dir:-"$script_dir/build/$arch"}
mkdir -p "$output_dir"
output_dir=$(cd -P -- "$output_dir" && pwd -P)

# Reuse a complete preparation until its inputs change or --refresh is requested.
fingerprint=$({ printf '%s\n' "$arch" "$profile"; cat "$script_dir/build.sh" \
    "$script_dir/build-rootfs.sh" "$script_dir/Dockerfile"; } | cksum)
if ! $refresh && [[ -f $output_dir/.prepared ]] &&
    [[ $(cat "$output_dir/.prepared") == "$fingerprint" ]] &&
    [[ -s $output_dir/rootfs.img && -s $output_dir/sysroot.tar &&
       -f $output_dir/rootfs/lib/apk/db/installed &&
       -f $output_dir/sysroot/usr/include/errno.h && -f $output_dir/sysroot/usr/lib/libc.so ]]; then
    echo "Using prepared Alpine $arch $profile at $output_dir"
    exit 0
fi

platform=$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')
case "$platform" in linux/*) ;; *) echo "A Linux Docker engine is required." >&2; exit 1 ;; esac
image="pedigree-alpine-rootfs:3.22-${platform#linux/}"
docker build --platform "$platform" -t "$image" "$script_dir"

staging=$(mktemp -d "$output_dir/.prepare.XXXXXX")
trap 'rm -rf "$staging"' EXIT
docker run --rm --platform "$platform" \
    -e "ALPINE_ARCH=$arch" -e "ALPINE_PROFILE=$profile" \
    -e "OUTPUT_UID=$(id -u)" -e "OUTPUT_GID=$(id -g)" \
    -v "$staging:/out" "$image"

rm -f "$output_dir/.prepared"
for artifact in rootfs sysroot rootfs.img sysroot.tar rootfs.packages sysroot.packages; do
    rm -rf "$output_dir/$artifact"
    mv "$staging/$artifact" "$output_dir/$artifact"
done
printf '%s\n' "$fingerprint" > "$output_dir/.prepared"
