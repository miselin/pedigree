#!/bin/bash

# Build Alpine ARM images on Apple silicon without a Linux container.
set -euo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
arch=${1:-aarch64}
case "$arch" in
    aarch64|armv7) ;;
    *) echo "Unsupported Alpine architecture: $arch" >&2; exit 1 ;;
esac
output_dir=${2:-"$script_dir/build/$arch"}
release=3.22.1
repository=https://dl-cdn.alpinelinux.org/alpine/v3.22

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
    echo "The native Alpine builder requires macOS arm64." >&2
    exit 1
fi

for command in bsdtar curl mkfile shasum; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command is unavailable: $command" >&2
        exit 1
    fi
done

for command in mke2fs debugfs; do
    if ! command -v "$command" >/dev/null 2>&1; then
        if [[ -x /opt/homebrew/sbin/$command ]]; then
            export PATH="/opt/homebrew/sbin:$PATH"
        else
            echo "Install Homebrew e2fsprogs for $command." >&2
            exit 1
        fi
    fi
done

mkdir -p "$output_dir"
output_dir=$(cd -P -- "$output_dir" && pwd -P)
work_dir=$(mktemp -d "$output_dir/.native-$arch.XXXXXX")
installed_sysroot=false
cleanup() {
    local status=$?
    if (( status != 0 )); then
        if $installed_sysroot; then
            rm -rf "$output_dir/sysroot"
        fi
        if [[ -e $work_dir/previous-sysroot || -L $work_dir/previous-sysroot ]]; then
            if ! mv "$work_dir/previous-sysroot" "$output_dir/sysroot"; then
                echo "Previous sysroot remains at $work_dir/previous-sysroot" >&2
                return 1
            fi
        fi
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT
root="$work_dir/root"
mkdir -p "$root"

curl -fLsS --retry 3 -o "$work_dir/minirootfs.tar.gz" \
    "$repository/releases/$arch/alpine-minirootfs-$release-$arch.tar.gz"
curl -fLsS --retry 3 -o "$work_dir/minirootfs.sha256" \
    "$repository/releases/$arch/alpine-minirootfs-$release-$arch.tar.gz.sha256"
(
    cd "$work_dir"
    mv minirootfs.tar.gz "alpine-minirootfs-$release-$arch.tar.gz"
    shasum -a 256 -c minirootfs.sha256
)
bsdtar -xzf "$work_dir/alpine-minirootfs-$release-$arch.tar.gz" -C "$root"

curl -fLsS --retry 3 -o "$work_dir/APKINDEX.tar.gz" \
    "$repository/main/$arch/APKINDEX.tar.gz"
bsdtar -xOf "$work_dir/APKINDEX.tar.gz" APKINDEX > "$work_dir/APKINDEX"

package_version() {
    awk -v wanted="$1" '
        /^P:/ { selected = ($0 == "P:" wanted); next }
        selected && /^V:/ { print substr($0, 3); exit }
    ' "$work_dir/APKINDEX"
}

musl_version=$(package_version musl)
musl_dev_version=$(package_version musl-dev)
headers_version=$(package_version linux-headers)
if [[ -z $musl_version || $musl_version != "$musl_dev_version" ||
      -z $headers_version ]]; then
    echo "Alpine musl and header packages are missing or inconsistent." >&2
    exit 1
fi

download_package() {
    local name=$1
    local version=$2
    if [[ ! $version =~ ^[a-zA-Z0-9._+-]+$ ]]; then
        echo "Invalid Alpine package version: $version" >&2
        exit 1
    fi
    curl -fLsS --retry 3 -o "$work_dir/$name.apk" \
        "$repository/main/$arch/$name-$version.apk"
}

download_package musl "$musl_version"
download_package musl-dev "$musl_dev_version"
download_package linux-headers "$headers_version"
bsdtar -xf "$work_dir/musl.apk" -C "$root" lib
bsdtar -xf "$work_dir/musl-dev.apk" -C "$root" usr
bsdtar -xf "$work_dir/linux-headers.apk" -C "$root" usr

# bsdtar clears sticky directory bits when extracting as an ordinary user.
chmod 1777 "$root/tmp" "$root/var/tmp"

printf 'pedigree\n' > "$root/etc/hostname"
printf '/dev/root / ext2 defaults 0 0\n' > "$root/etc/fstab"
printf 'ttyS0::respawn:/bin/sh -i\n' > "$root/etc/inittab"
mkdir -p "$root/run/sockets"
rm -f "$root/etc/securetty"
sed 's/^root:[^:]*:/root::/' "$root/etc/shadow" > "$work_dir/shadow"
cat "$work_dir/shadow" > "$root/etc/shadow"

mkdir -p "$work_dir/sysroot/usr"
cp -pR "$root/usr/include" "$root/usr/lib" "$work_dir/sysroot/usr/"
cp -pR "$root/lib" "$work_dir/sysroot/"

mkfile -n 128m "$work_dir/rootfs.img"
mke2fs -q -t ext2 -F -b 4096 -L rootfs -m 0 -d "$root" \
    -U 50e5c7c0-b79c-4932-8cdc-c2b2c713ff97 "$work_dir/rootfs.img"

# Extraction as an ordinary macOS user changes ownership to that user.
# Correct the image metadata without requiring root privileges on the host.
find "$root" -mindepth 1 -print0 |
    while IFS= read -r -d '' entry; do
        path=${entry#"$root"}
        if [[ $path == *'"'* || $path == *$'\n'* ]]; then
            echo "Unsupported path in Alpine image: $path" >&2
            exit 1
        fi
        printf 'sif "%s" uid 0\nsif "%s" gid 0\n' "$path" "$path"
    done > "$work_dir/ownership.commands"
printf 'sif "/etc/shadow" gid 42\n' >> "$work_dir/ownership.commands"
debugfs -w -f "$work_dir/ownership.commands" "$work_dir/rootfs.img" \
    > "$work_dir/debugfs.log" 2>&1
if grep -E 'File not found|Command not found|while setting inode|Invalid argument' \
    "$work_dir/debugfs.log" >&2; then
    echo "Failed to set Alpine image ownership." >&2
    exit 1
fi

if [[ -e $output_dir/sysroot || -L $output_dir/sysroot ]]; then
    mv "$output_dir/sysroot" "$work_dir/previous-sysroot"
fi
mv "$work_dir/sysroot" "$output_dir/sysroot"
installed_sysroot=true
mv -f "$work_dir/rootfs.img" "$output_dir/rootfs.img"
