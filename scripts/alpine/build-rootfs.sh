#!/bin/sh
set -eu

arch=${ALPINE_ARCH:-x86_64}
profile=${ALPINE_PROFILE:-base}
case "$arch" in x86_64|aarch64|armv7) ;; *) echo "Unsupported architecture: $arch" >&2; exit 1 ;; esac
case "$profile" in base|desktop) ;; *) echo "Unsupported profile: $profile" >&2; exit 1 ;; esac

repository=https://dl-cdn.alpinelinux.org/alpine/v3.22
release=3.22.1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root="$work/rootfs"
sdk="$work/sdk"
mkdir -p "$root"

archive="alpine-minirootfs-$release-$arch.tar.gz"
curl -fLsS --retry 3 -o "$work/$archive" "$repository/releases/$arch/$archive"
curl -fLsS --retry 3 -o "$work/$archive.sha256" "$repository/releases/$arch/$archive.sha256"
(cd "$work" && sha256sum -c "$archive.sha256")
tar -xzf "$work/$archive" -C "$root"
printf '%s/main\n%s/community\n' "$repository" "$repository" > "$root/etc/apk/repositories"

runtime_packages=
development_packages="musl-dev linux-headers"
if [ "$profile" = desktop ]; then
    runtime_packages="bash libgcc libstdc++ libintl dialog libpng freetype fontconfig
        pixman cairo expat mesa mesa-egl mesa-gl mesa-gles gettext pango glib pcre
        harfbuzz libffi libprotobuf"
    development_packages="$development_packages gettext-dev ncurses-dev libpng-dev
        freetype-dev fontconfig-dev pixman-dev cairo-dev expat-dev mesa-dev
        pango-dev glib-dev pcre-dev harfbuzz-dev libffi-dev protobuf-dev"
fi

# The host apk resolves target packages and verifies them with the target's keys.
# Package scripts must never execute binaries from the foreign architecture.
target_apk() {
    target=$1
    shift
    apk --root "$target" --arch "$arch" --keys-dir "$target/etc/apk/keys" \
        --repositories-file "$target/etc/apk/repositories" --cache-max-age 1440 "$@"
}
target_apk "$root" update
target_apk "$root" upgrade --no-self-upgrade --no-scripts --no-commit-hooks
# Word splitting is intentional for these fixed package lists.
if [ -n "$runtime_packages" ]; then
    target_apk "$root" add --no-scripts --no-commit-hooks $runtime_packages
fi
# The retained x86_64-pedigree compiler and existing binaries use this path.
if [ "$arch" = x86_64 ]; then
    ln -s ../../lib/ld-musl-x86_64.so.1 "$root/usr/lib/ld-musl-x86_64.so.1"
fi
cp -a "$root" "$sdk"
target_apk "$sdk" add --no-scripts --no-commit-hooks $development_packages
target_apk "$root" info -v > "$work/rootfs.packages"
target_apk "$sdk" info -v > "$work/sysroot.packages"
sort "$work/rootfs.packages" > /out/rootfs.packages
sort "$work/sysroot.packages" > /out/sysroot.packages
if [ -n "$(comm -23 /out/rootfs.packages /out/sysroot.packages)" ]; then
    echo "Runtime and development package versions disagree; retry preparation." >&2
    exit 1
fi

printf 'pedigree\n' > "$root/etc/hostname"
printf '/dev/root / ext2 defaults 0 0\n' > "$root/etc/fstab"
printf 'ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100\n' > "$root/etc/inittab"
mkdir -p "$root/run/sockets"
# The development image permits local root login without a password.
rm -f "$root/etc/securetty"
sed -i 's/^root:[^:]*:/root::/' "$root/etc/shadow"
chmod 1777 "$root/tmp" "$root/var/tmp"
rm -f "$root/var/cache/apk/"*

mkdir -p "$work/sysroot/usr" /out/sysroot
cp -a "$sdk/usr/include" "$sdk/usr/lib" "$sdk/usr/bin" "$work/sysroot/usr/"
cp -a "$sdk/lib" "$work/sysroot/"
tar -cpf /out/sysroot.tar -C "$work/sysroot" .
: > /out/.case-check-A
if [ -e /out/.case-check-a ]; then
    collisions=$(cd "$work/sysroot" && find . -print | LC_ALL=C sort -f | uniq -Di)
    if [ -n "$collisions" ]; then
        printf '%s\n%s\n' \
            'Case-insensitive SDK export merges the paths below; sysroot.tar preserves every header:' \
            "$collisions" >&2
    fi
fi
rm /out/.case-check-A
tar --overwrite -xpf /out/sysroot.tar -C /out/sysroot

# Generate the image before exporting directories through a host bind mount,
# which can map Linux package ownership to the host user's identity.
size_mib=$(( ($(du -sk "$root" | cut -f1) + 1023) / 1024 + 64 ))
if [ "$size_mib" -lt 128 ]; then size_mib=128; fi
truncate -s "${size_mib}M" /out/rootfs.img
mke2fs -q -t ext2 -F -b 4096 -L rootfs -m 0 -d "$root" \
    -U 50e5c7c0-b79c-4932-8cdc-c2b2c713ff97 /out/rootfs.img
cp -a "$root" /out/rootfs
chown -R "${OUTPUT_UID:-0}:${OUTPUT_GID:-0}" /out
