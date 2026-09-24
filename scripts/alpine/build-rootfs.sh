#!/bin/sh
set -eux

ROOT=/tmp/rootfs
IMG=/out/rootfs.img
ALPINE_ARCH=${ALPINE_ARCH:-x86_64}

case "$ALPINE_ARCH" in
    x86_64|aarch64|armv7) ;;
    *) echo "Unsupported Alpine architecture: $ALPINE_ARCH" >&2; exit 1 ;;
esac

mkdir -p "$ROOT"

# Either unpack Alpine's minirootfs...
curl -LO "https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/$ALPINE_ARCH/alpine-minirootfs-3.22.1-$ALPINE_ARCH.tar.gz"
tar -xzf "alpine-minirootfs-3.22.1-$ALPINE_ARCH.tar.gz" -C "$ROOT"

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
if [ "$ALPINE_ARCH" = x86_64 ]; then
    apk \
        --root "$ROOT" \
        --initdb \
        --repositories-file /etc/apk/repositories \
        add xorg-server xf86-video-fbdev xf86-input-evdev xinit

    mkdir -p "$ROOT/etc/X11/xorg.conf.d"
    cat > "$ROOT/etc/X11/xorg.conf.d/10-pedigree-input.conf" <<EOF
Section "ServerFlags"
    Option "AutoAddDevices" "false"
EndSection

Section "InputDevice"
    Identifier "Pedigree Keyboard"
    Driver "evdev"
    Option "Device" "/dev/input/event0"
    Option "CoreKeyboard"
EndSection

Section "InputDevice"
    Identifier "Pedigree Pointer"
    Driver "evdev"
    Option "Device" "/dev/input/event1"
    Option "CorePointer"
EndSection
EOF
else
    apk \
        --root "$ROOT" \
        --initdb \
        --repositories-file /etc/apk/repositories \
        add musl-dev linux-headers

    cat > "$ROOT/etc/inittab" <<EOF
ttyS0::respawn:/bin/sh -i
EOF

    mkdir -p /out/sysroot/usr
    cp -a "$ROOT/usr/include" /out/sysroot/usr/
    cp -a "$ROOT/usr/lib" /out/sysroot/usr/
    cp -a "$ROOT/lib" /out/sysroot/
fi

if [ "$ALPINE_ARCH" = aarch64 ] || [ "$ALPINE_ARCH" = armv7 ]; then
    truncate -s 128M "$IMG"
else
    truncate -s 512M "$IMG"
fi

# Use the same UUID as scripts/create_diskimage.py for the embedded command-line to choose the right rootfs
mke2fs \
    -t ext2 \
    -F \
    -b 4096 \
    -L rootfs \
    -m 0 \
    -d "$ROOT" \
    -U "50e5c7c0-b79c-4932-8cdc-c2b2c713ff97" \
    "$IMG"
