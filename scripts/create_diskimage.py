"""
Copyright (c) 2008-2014, Pedigree Developers

Please see the CONTRIB file in the root of the source tree for a full
list of contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
"""

from __future__ import print_function

import os
import stat
import sys
import subprocess
import tempfile


# Historical packages and the old build sysroot use Pedigree's original
# directory names. New images use a conventional usr-merged FHS layout.
LEGACY_TARGET_PREFIXES = (
    ("/.profile", "/root/.profile"),
    ("/.bashrc", "/root/.bashrc"),
    ("/support/pup/db", "/var/cache/pup"),
    ("/support/pup", "/etc/pup"),
    ("/system/initscripts", "/etc/init.d"),
    ("/system/modules", "/usr/lib/modules"),
    ("/system/include", "/usr/include"),
    ("/system/locale", "/usr/share/locale"),
    ("/system/keymaps", "/usr/share/keymaps"),
    ("/system/fonts", "/usr/share/fonts"),
    ("/applications", "/usr/bin"),
    ("/libraries", "/usr/lib"),
    ("/initscripts", "/etc/init.d"),
    ("/config", "/etc"),
    ("/support", "/usr/lib/pedigree"),
    ("/include", "/usr/include"),
    ("/users", "/home"),
    ("/fonts", "/usr/share/fonts"),
    ("/docs", "/usr/share/doc"),
    ("/doc", "/usr/share/doc"),
)


def translate_target_path(path):
    for legacy, fhs in LEGACY_TARGET_PREFIXES:
        if path == legacy or path.startswith(legacy + "/"):
            return fhs + path[len(legacy) :]

    return path


def silent_makedirs(p):
    """Variant of makedirs that doesn't error if the full path exists."""
    if not os.path.exists(p):
        os.makedirs(p)


def build_user_map(baseimagesdir):
    # Read the same account files that will be installed in the image.
    users = {
        # UID, default GID.
        "root": (0, 0),
    }
    groups = {
        "root": 0,
    }

    passwd_path = os.path.join(baseimagesdir, "etc/passwd")
    if os.path.isfile(passwd_path):
        with open(passwd_path) as passwd:
            for line in passwd:
                fields = line.rstrip("\r\n").split(":")
                if len(fields) >= 4:
                    users[fields[0]] = (int(fields[2]), int(fields[3]))

    group_path = os.path.join(baseimagesdir, "etc/group")
    if os.path.isfile(group_path):
        with open(group_path) as group:
            for line in group:
                fields = line.rstrip("\r\n").split(":")
                if len(fields) >= 3:
                    groups[fields[0]] = int(fields[2])

    return users, groups


def add_copy(copylist, source, target, override=False):
    """Adds a copy to the given copy list, handling duplicates correctly"""
    target = translate_target_path(target)
    entry = copylist.get(source)
    if entry and not override:
        entry.add(target)
    else:
        copylist[source] = set([target])


def target_in_copylist(copylist, target):
    for value in copylist.values():
        if target in value:
            return True

    return False


def add_copy_tree(
    copylist, base_dir, target_prefix="", replacements=None, extensions=None
):
    for dirpath, dirs, files in os.walk(base_dir):
        target_path = dirpath.replace(base_dir, "")

        if target_prefix:
            target_path = os.path.join(target_prefix, target_path.lstrip("/"))
        elif not target_path:
            target_path = "/"

        if replacements:
            for r in replacements:
                target_path = target_path.replace(*r)

        for f in files:
            target_fullpath = os.path.join(target_path, f)
            source_fullpath = os.path.join(dirpath, f)
            ok = True
            if extensions:
                ok = False
                for ext in extensions:
                    if source_fullpath.endswith("." + ext):
                        ok = True
                        break

            if not ok:
                continue

            add_copy(copylist, source_fullpath, target_fullpath, True)


def add_file_to_cmdlist(cmdlist, source, target):
    if os.path.isfile(source):
        cmdlist.append("write %s %s" % (source, target))

        # Figure out if we need executable permission or not.
        # We assume the default (0644) is acceptable otherwise.
        mode = os.stat(source).st_mode
        if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            cmdlist.append("chmod %s 755" % (target,))


def safe_mkdirs_cmdlist(cmdlist, d, safe_dirs):
    if not d or d == "/":
        return  # already exists

    if d not in safe_dirs:
        safe_mkdirs_cmdlist(cmdlist, os.path.dirname(d), safe_dirs)
        cmdlist.append("mkdir %s" % (d,))
        safe_dirs.add(d)


def build_file_list(all_sources):
    """Builds a full command list for ext2img."""
    (
        imagesdir,
        srcdir,
        baseimagesdir,
        kernel,
        initrd,
        configdb,
        grublst,
        musldir,
        binarydir,
    ) = all_sources[:9]
    additional_sources = all_sources[9:]

    users, groups = build_user_map(baseimagesdir)

    # Host path -> Pedigree path mapping.
    copies = {}

    add_copy(copies, configdb, "/.pedigree-root")
    add_copy(copies, grublst, "/boot/grub/menu.lst")
    add_copy(copies, kernel, "/boot/kernel")
    if initrd != "__noinitrd__":
        add_copy(copies, initrd, "/boot/initrd.tar")

    for source in additional_sources:
        prefix = "/"
        basename = os.path.basename(source)
        dirname = os.path.dirname(source)

        if dirname.endswith("src/user"):
            if basename.startswith("lib") and basename.endswith(".so"):
                prefix = "/usr/lib"
            else:
                prefix = "/usr/bin"
        elif dirname.endswith("src/modules"):
            if basename.startswith("lib") and basename.endswith(".so"):
                prefix = "/usr/lib"
            else:
                prefix = "/usr/lib/modules"

        add_copy(copies, source, os.path.join(prefix, basename))

    add_copy_tree(
        copies,
        baseimagesdir,
        replacements=(
            ("/config/term", "/usr/lib/pedigree/ncurses/share"),
            ("/.profile", "/root/.profile"),
            ("/.bashrc", "/root/.bashrc"),
        ),
    )
    add_copy_tree(copies, os.path.join(musldir, "lib"), "/usr/lib")
    add_copy_tree(copies, os.path.join(musldir, "include"), "/usr/include")

    # Add translations.
    for lang in ("en_US", "de_DE"):
        add_copy_tree(
            copies,
            os.path.join(binarydir, "src/po/" + lang),
            "/usr/share/locale/" + lang + ".UTF-8/LC_MESSAGES",
            extensions=("gmo",),
        )

    # Add keymaps.
    add_copy_tree(copies, os.path.join(binarydir, "keymaps"), "/usr/share/keymaps")

    # Build command list.
    cmdlist = []
    safe_dirs = set()
    for fhs_dir in (
        "/dev",
        "/etc",
        "/home",
        "/media",
        "/proc",
        "/root",
        "/run",
        "/tmp",
        "/usr/bin",
        "/usr/include",
        "/usr/lib",
        "/usr/sbin",
        "/usr/share",
        "/var/cache",
        "/var/run",
    ):
        safe_mkdirs_cmdlist(cmdlist, fhs_dir, safe_dirs)

    for dirpath, dirs, files in os.walk(imagesdir):
        legacy_dirpath = dirpath.replace(imagesdir, "")
        if not legacy_dirpath:
            legacy_dirpath = "/"
        target_dirpath = translate_target_path(legacy_dirpath)

        safe_mkdirs_cmdlist(cmdlist, target_dirpath, safe_dirs)

        changedDefaults = False
        if target_dirpath.startswith("/home/"):
            user = target_dirpath.split("/")[2]
            if user in users:
                cmdlist.append("defaultowner %d %d" % users[user])
                changedDefaults = True

        for d in dirs:
            target = translate_target_path(os.path.join(legacy_dirpath, d))
            safe_mkdirs_cmdlist(cmdlist, target, safe_dirs)

            if target_dirpath == "/home":
                if d in users:
                    cmdlist.append(
                        "chown %s %d %d" % (target, users[d][0], users[d][1])
                    )

        for f in sorted(files):
            source = os.path.join(dirpath, f)

            # This file might need to be copied from the build directory.
            target = os.path.join(target_dirpath, f)
            if target_in_copylist(copies, target):
                print(
                    "Target %s will be overridden by files in the build directory."
                    % (target,)
                )
                continue

            if os.path.islink(source):
                link_target = os.readlink(source)
                if link_target.startswith(dirpath):
                    link_target = link_target.replace(dirpath, "").lstrip("/")
                elif link_target.startswith("/"):
                    link_target = translate_target_path(link_target)

                cmdlist.append("symlink %s %s" % (target, link_target))
            elif os.path.isfile(source):
                add_file_to_cmdlist(cmdlist, source, target)

        if changedDefaults:
            cmdlist.append("defaultowner 0 0")

    for host_path, target_paths in copies.items():
        for target_path in target_paths:
            dirname = os.path.dirname(target_path)
            if dirname not in safe_dirs:
                safe_mkdirs_cmdlist(cmdlist, dirname, safe_dirs)

            if os.path.isfile(host_path):
                add_file_to_cmdlist(cmdlist, host_path, target_path)
            else:
                raise Exception(
                    'Host file "%s" for target path %s is not a file.'
                    % (host_path, target_path)
                )

    cmdlist.append("chmod /etc/shadow 600")

    # Add some more useful layout features (e.g. to make /bin/sh work).
    cmdlist.append("symlink /usr/bin/sh /usr/bin/bash")
    cmdlist.append("symlink /bin /usr/bin")
    cmdlist.append("symlink /lib /usr/lib")
    cmdlist.append("symlink /sbin /usr/sbin")

    # Sort the command lists so we do everything in batches (e.g. mkdir, chmod)
    def count_components(path):
        return path.count("/")

    def commandlist_sorter(item):
        order = ["mkdir", "write", "symlink", "chmod"]
        item_components = item.split()
        item_which = item_components[0]
        if item_which in ("mkdir", "write", "symlink"):
            item_target_path = item_components[-1]
        else:
            item_target_path = item_components[1]

        # First key - ordered commands. Second key - # of path components.
        return (order.index(item_which), count_components(item_target_path))

    return list(sorted(cmdlist, key=commandlist_sorter))


def create_base_image(target):
    # Offset into the image for the partition proper to start.
    partition_offset = 0  # 0x10000

    # Build file for creating the disk image.
    base_image = open(target, "w")

    # Create image - 1GiB.
    sz = (1 << 31) + partition_offset
    base_image.truncate(sz)

    # Add a partition table to the front of the image.
    if partition_offset:
        hpc = 16  # Heads per cylinder
        spt = 63  # Sectors per track

        # LBA sector count.
        lba = sz // 512
        end_cyl = lba // (spt * hpc)
        end_head = (lba // spt) % hpc
        end_sector = (lba % spt) + 1

        # Sector start LBA.
        start_lba = partition_offset // 512
        start_cyl = start_lba // (spt * hpc)
        start_head = (start_lba // spt) % hpc
        start_sector = (start_lba % spt) + 1

        # Partition entry.
        entry = "\x80"  # Partition is active/bootable.
        entry += struct.pack(
            "BBB", start_head & 0xFF, start_sector & 0xFF, start_cyl & 0xFF
        )
        entry += "\x83"  # ext2 partition - ie, a Linux native filesystem.
        entry += struct.pack("BBB", end_head & 0xFF, end_sector & 0xFF, end_cyl & 0xFF)
        entry += struct.pack("=L", start_lba)
        entry += struct.pack("=L", lba)

        # Build partition table.
        partition = "\x00" * 446
        partition += entry
        partition += "\x00" * 48
        partition += "\x55\xaa"
        base_image.write(partition)

    base_image.close()

    mke2fs = "/sbin/mke2fs"
    if sys.platform == "darwin":
        mke2fs = "/opt/homebrew/sbin/mke2fs"

    # Generate ext2 filesystem.
    args = [
        mke2fs,  # TODO(miselin): need to detect in CMake and pass path
        "-q",
    ]
    if partition_offset:
        args += [
            "-E",  # Don't use UID/GID from host system.
            "offset=%d" % partition_offset,
        ]
    args += [
        "-O",
        "^dir_index",  # Don't (yet) use directory b-trees.
        "-I",
        "128",  # Use 128-byte inodes, as grub-legacy can't use bigger.
        "-F",
        "-L",
        "pedigree",
        target,
    ]
    subprocess.check_call(args)


def main():
    targetfile = sys.argv[1]
    ext2img = sys.argv[2]
    sources = sys.argv[3:]
    cmdlist = build_file_list(sources)

    create_base_image(targetfile)

    with open("/tmp/cmdlist", "w") as f:
        f.write("\n".join(cmdlist))

    # Dump our files into the image using ext2img (built as part of the normal
    # Pedigree build, to run on the build system - not on Pedigree).
    with tempfile.NamedTemporaryFile() as f:
        f.write("\n".join(cmdlist).encode("utf-8"))
        f.flush()

        args = [
            ext2img,
            "-q",
            "-c",
            f.name,
            "-f",
            targetfile,
        ]

        subprocess.check_call(args)


if __name__ == "__main__":
    main()
