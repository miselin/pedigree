#!/bin/sh
set -eux

ROOT=/tmp/rootfs
IMG=/out/rootfs.img

mkdir -p "$ROOT"

# Either unpack Alpine's minirootfs...
curl -LO https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/alpine-minirootfs-3.22.1-x86_64.tar.gz
tar -xzf alpine-minirootfs-*.tar.gz -C "$ROOT"

# Your customisation:
echo pedigree > "$ROOT/etc/hostname"

cat > "$ROOT/etc/fstab" <<EOF
/dev/root / ext2 defaults 0 0
EOF

# Pedigree needs this
mkdir -p "$ROOT/run/sockets"

# hacks to work around issues with ptsname/ttyname
rm -f "$ROOT/etc/securetty"

sed -i 's/^root:[^:]*:/root::/' "$ROOT/etc/shadow"

# fun fun fun
apk \
    --root "$ROOT" \
    --initdb \
    --repositories-file /etc/apk/repositories \
    add xorg-server xf86-video-fbdev xinit

# e.g. 512 MiB image
truncate -s 512M "$IMG"

# Use the same UUID as scripts/create_diskimage.py for the embedded command-line to choose the right rootfs
mke2fs \
    -t ext2 \
    -F \
    -L rootfs \
    -m 0 \
    -d "$ROOT" \
    -U "50e5c7c0-b79c-4932-8cdc-c2b2c713ff97" \
    "$IMG"
