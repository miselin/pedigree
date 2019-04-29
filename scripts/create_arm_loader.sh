#!/bin/bash

out=$1
uboot=$2
build_path=$(realpath $3)

set -e

mkdir -p "$build_path/bits"
bits="$build_path/bits"

if [ "x$out$uboot$build_path" = "x" ]; then
    echo "usage: create_arm_loader.sh output_image uboot_dir"
    exit 1
fi

# TODO: create ext2 partition on the virtual SD card again
dd if=/dev/zero of="$bits/out.img" bs=4096 count=64000  # 256M

# Create the partition table on the target image
fdisk -c=dos -u=cylinders "$bits/out.img" <<END
o
n
p
1
1
+64M
a
t
c
w
END
fdisk -l "$bits/out.img"

# Pedigree boot script.
cat <<EOF >"$bits/pedigree.txt"
setenv initrd_high "0xffffffff"
setenv fdt_high "0xffffffff"
setenv bootcmd "fatload mmc 0:1 0x88000000 uImage;bootm 88000000"
boot
EOF

cat <<EOF >"$bits/uEnv.txt"
bootenv=boot.scr
loaduimage=fatload mmc \${mmcdev} \${loadaddr} \${bootenv}
mmcboot=echo Running boot.scr script from mmc ...; source \${loadaddr}
EOF

mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n 'Booting Pedigree...' \
    -d "$bits/pedigree.txt" "$bits/boot.scr"

echo "Generating FAT filesystem..."

# Create FAT filesystem.
cat <<EOF >.mtoolsrc
drive z: file="$bits/out.img" fat_bits=32 cylinders=50 heads=255 sectors=63 partition=1 mformat_only
EOF

export MTOOLSRC="$PWD/.mtoolsrc"

# Copy files into the FAT partition.
mformat z:
mcopy "$uboot/MLO" z:
mcopy "$uboot/u-boot.img" "$bits/boot.scr" "$bits/uEnv.txt" z:
mcopy "$build_path/beagle_uImage" z:uImage

rm -f .mtoolsrc

mv "$bits/out.img" "$out"

echo "$out is ready for booting."
