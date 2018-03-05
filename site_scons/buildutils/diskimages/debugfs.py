'''
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
'''

import os
import shutil
import subprocess
import stat
import struct
import sqlite3
import tempfile

import SCons


def buildImageE2fsprogs(target, source, env):
    if env['verbose']:
        print('      Creating ' + os.path.basename(target[0].path))
    else:
        print('      Creating \033[32m' + os.path.basename(target[0].path) + '\033[0m')

    builddir = env["PEDIGREE_BUILD_BASE"]
    imagedir = env['PEDIGREE_IMAGES_DIR']
    appsdir = env['PEDIGREE_BUILD_APPS']
    modsdir = env['PEDIGREE_BUILD_MODULES']
    drvsdir = env['PEDIGREE_BUILD_DRIVERS']
    libsdir = builddir.Dir('libs')
    i18ndir = builddir.Dir('po').Dir('locale')
    keymaps_dir = env['HOST_BUILDDIR'].Dir('keymaps')

    outFile = target[0].path

    # ext2img inserts files into our disk image.
    ext2img = os.path.join(env['HOST_BUILDDIR'].path, 'ext2img')

    # TODO(miselin): this image will not have GRUB on it.

    # Host path -> Pedigree path.
    builddir_copies = {}

    # Adds a copy to builddir_copies, handling duplicates correctly.
    def add_builddir_copy(source, target, override=False):
        if isinstance(source, SCons.Node.FS.Base):
            source = source.path

        entry = builddir_copies.get(source)
        if entry and not override:
            entry.add(target)
        else:
            builddir_copies[source] = set([target])

    def target_in_copies(target):
        for value in builddir_copies.values():
            if target in value:
                return True

        return False

    # Copy files into the local images directory, ready for creation.
    add_builddir_copy(builddir.File('config.db'), '/.pedigree-root')

    # Open the configuration database and collect known users and groups.
    try:
        conn = sqlite3.connect(builddir.File('config.db'))
    except:
        # Won't be able to assign permissions correctly.
        conn = None

    users = {
        # UID, default GID.
        'root': (0, 0),
    }
    groups = {
        'root': 0,
    }

    if conn is not None:
        q = conn.execute('select gid, name from groups')
        for row in q:
            groups[row[1]] = int(row[0]) - 1

        q = conn.execute('select uid, username, groupname from users')
        for row in q:
            users[row[1]] = (int(row[0]) - 1, groups[row[2]])

        conn.close()

    def makedirs(p):
        if not os.path.exists(p):
            os.makedirs(p)

    # Add GRUB config.
    add_builddir_copy(imagedir.Dir('..').Dir('grub').File('menu-hdd.lst').path, '/boot/grub/menu.lst')

    # Copy the kernel, initrd, and configuration database
    if env['kernel_on_disk']:
        nth = 3
        if 'STATIC_DRIVERS' in env['CPPDEFINES']:
            nth = 2
        for i in source[0:nth]:
            add_builddir_copy(i.path, '/boot/' + i.name)
    else:
        nth = 0
    source = source[nth:]

    # Copy each input file across
    for i in source:
        prefix = '/'

        # Applications
        if appsdir.abspath in i.abspath:
            prefix = '/applications'

        # Modules
        elif modsdir.abspath in i.abspath:
            prefix = '/system/modules'

        # Drivers
        elif drvsdir.abspath in i.abspath:
            prefix = '/system/modules'

        # User Libraries
        elif libsdir.abspath in i.abspath:
            prefix = '/libraries'

        # Additional Libraries
        elif builddir.abspath in i.abspath:
            prefix = '/libraries'

        # Already in the image.
        elif imagedir.abspath in i.abspath:
            continue

        # Clean out the last directory name if needed
        add_builddir_copy(i.path, os.path.join(prefix, i.name))

    def extra_copy_tree(base_dir, target_prefix='', replacements=None):
        for (dirpath, dirs, files) in os.walk(base_dir):
            target_path = dirpath.replace(base_dir, '')

            if target_prefix:
                target_path = os.path.join(target_prefix,
                                           target_path.lstrip('/'))
            elif not target_path:
                target_path = '/'

            if replacements:
                for r in replacements:
                    target_path = target_path.replace(*r)

            for f in files:
                target_fullpath = os.path.join(target_path, f)
                source_fullpath = os.path.join(dirpath, f)
                add_builddir_copy(source_fullpath, target_fullpath, True)

    # Copy etc bits.
    base_dir = imagedir.Dir('..').Dir('base')
    extra_copy_tree(base_dir.path, replacements=(
        ('/config/term', '/support/ncurses/share'),
    ))

    # Copy locale files from user apps.
    if env['build_translations']:
        extra_copy_tree(i18ndir.path, target_prefix='/system/locale')

    # Copy keymaps.
    extra_copy_tree(keymaps_dir.path, target_prefix='/system/keymaps')

    # Copy musl, if we can.
    if env['posix_musl']:
        extra_copy_tree(builddir.Dir('musl').Dir('lib').path, '/libraries')
        extra_copy_tree(builddir.Dir('musl').Dir('include').path,
                        '/system/include')

    # Offset into the image for the partition proper to start.
    partition_offset = 0 # 0x10000

    # Build file for creating the disk image.
    base_image = open(outFile, 'w')

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
        entry = '\x80'  # Partition is active/bootable.
        entry += struct.pack('BBB',
                             start_head & 0xFF,
                             start_sector & 0xFF,
                             start_cyl & 0xFF)
        entry += '\x83'  # ext2 partition - ie, a Linux native filesystem.
        entry += struct.pack('BBB',
                             end_head & 0xFF,
                             end_sector & 0xFF,
                             end_cyl & 0xFF)
        entry += struct.pack('=L', start_lba)
        entry += struct.pack('=L', lba)

        # Build partition table.
        partition = '\x00' * 446
        partition += entry
        partition += '\x00' * 48
        partition += '\x55\xAA'
        base_image.write(partition)

    base_image.close()

    # Generate ext2 filesystem.
    args = [
        env['MKE2FS'],
        '-q',
    ]
    if partition_offset:
        args += [
            '-E', 'offset=%d' % partition_offset,  # Don't use UID/GID from host system.
        ]
    args += [
        '-O', '^dir_index',  # Don't (yet) use directory b-trees.
        '-I', '128',  # Use 128-byte inodes, as grub-legacy can't use bigger.
        '-F',
        '-L',
        'pedigree',
        outFile,
    ]
    subprocess.check_call(args)

    def add_file(cmdlist, source, target):
        if os.path.isfile(source):
            cmdlist.append('write %s %s' % (source, target))

            # Figure out if we need executable permission or not.
            # We assume the default (0644) is acceptable otherwise.
            mode = os.stat(source).st_mode
            if mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
                cmdlist.append('chmod %s 755' % (target,))

    # Populate the image.
    cmdlist = []
    safe_dirs = set()
    for (dirpath, dirs, files) in os.walk(imagedir.path):
        target_dirpath = dirpath.replace(imagedir.path, '')
        if not target_dirpath:
            target_dirpath = '/'

        safe_dirs.add(target_dirpath)

        changedDefaults = False
        if target_dirpath.startswith('/users/'):
            user = target_dirpath.split('/')[2]
            if user in users:
                cmdlist.append('defaultowner %d %d' % users[user])
                changedDefaults = True

        for d in dirs:
            target = os.path.join(target_dirpath, d)
            cmdlist.append('mkdir %s' % (target,))

            if target_dirpath == '/users':
                if d in users:
                    cmdlist.append('chown %s %d %d' % (target, users[d][0], users[d][1]))

        for f in sorted(files):
            source = os.path.join(dirpath, f)

            # This file might need to be copied from the build directory.
            target = os.path.join(target_dirpath, f)
            if target_in_copies(target):
                print('Target %s will be overridden by files in the build directory.' % (target,))
                continue

            if os.path.islink(source):
                link_target = os.readlink(source)
                if link_target.startswith(dirpath):
                    link_target = link_target.replace(dirpath, '').lstrip('/')

                cmdlist.append('symlink %s %s' % (target, link_target))
            elif os.path.isfile(source):
                add_file(cmdlist, source, target)

        if changedDefaults:
            cmdlist.append('defaultowner 0 0')

    def safe_mkdirs(d):
        if d not in safe_dirs:
            safe_mkdirs(os.path.dirname(d))
            cmdlist.append('mkdir %s' % (d,))
            safe_dirs.add(d)

    for host_path, target_paths in builddir_copies.items():
        for target_path in target_paths:
            dirname = os.path.dirname(target_path)
            if dirname not in safe_dirs:
                safe_mkdirs(dirname)

            if os.path.isfile(host_path):
                add_file(cmdlist, host_path, target_path)
            else:
                raise Exception('Host file "%s" for target path %s is not a file.' % (host_path, target_path))

    # Add some more useful layout features (e.g. to make /bin/sh work).
    cmdlist.append('symlink /applications/sh /applications/bash')

    # Sort the command lists so we do everything in batches (e.g. mkdir, chmod)
    def count_components(path):
        return path.count('/')

    def commandlist_sorter(item):
        order = ['mkdir', 'write', 'symlink', 'chmod']
        item_components = item.split()
        item_which = item_components[0]
        if item_which in ('mkdir', 'write', 'symlink'):
            item_target_path = item_components[-1]
        else:
            item_target_path = item_components[1]

        # First key - ordered commands. Second key - # of path components.
        return (order.index(item_which), count_components(item_target_path))

    cmdlist = sorted(cmdlist, key=commandlist_sorter)

    with open('/tmp/cmdlist', 'w') as f:
        f.write('\n'.join(cmdlist))

    # Dump our files into the image using ext2img (built as part of the normal
    # Pedigree build, to run on the build system - not on Pedigree).
    with tempfile.NamedTemporaryFile() as f:
        f.write('\n'.join(cmdlist))
        f.flush()

        args = [
            ext2img,
            '-q',
            '-c',
            f.name,
            '-f',
            outFile,
        ]

        subprocess.check_call(args, cwd=env.Dir('#').abspath)
