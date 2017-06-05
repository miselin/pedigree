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

import debugfs
import losetup
import livecd
import mtools
import targetdir
import post

import SCons


def buildDiskImages(env, config_database):
    builddir = env["PEDIGREE_BUILD_BASE"]
    imagedir = env['PEDIGREE_IMAGES_DIR']

    hddimg = builddir.File('hdd.img')
    cdimg = builddir.File('pedigree.iso')

    if (env['ARCH_TARGET'] not in ['X86', 'X64', 'PPC', 'ARM', 'HOSTED'] or
            not env['build_images']):
        print 'No disk images being built.'
        return

    # We depend on the ext2img host tool to inject files into ext2 images.
    ext2img = env['HOST_BUILDDIR'].File('ext2img')
    env.Depends(hddimg, ext2img)

    env.Depends(hddimg, 'libs')
    if env['build_translations']:
        env.Depends(hddimg, 'i18n')
    env.Depends(hddimg, 'keymaps')

    if env['ARCH_TARGET'] != 'ARM':
        env.Depends(hddimg, 'apps')

    env.Depends(hddimg, config_database)

    if env['iso']:
        env.Depends(cdimg, hddimg) # Inherent dependency on libs/apps

    fileList = []

    kernel = builddir.Dir('kernel').File('kernel')
    initrd = builddir.File('initrd.tar')

    apps = builddir.Dir('apps')
    modules = builddir.Dir('modules')
    drivers = builddir.Dir('drivers')

    if env['posix_musl']:
        libc = builddir.Dir('musl').Dir('lib').File('libc.so')
        image_c_libs = [libc]
    else:
        libc = builddir.File('libc.so')
        libm = builddir.File('libm.so')
        libpthread = builddir.File('libpthread.so')
        image_c_libs = [libc, libm, libpthread]

        # TODO(miselin): more ARM userspace
        if env['ARCH_TARGET'] != 'ARM':
            libload = builddir.File('libload.so')
            image_c_libs.append(libload)

    if env['ARCH_TARGET'] != 'ARM':
        libui = builddir.Dir('libs').File('libui.so')
        libtui = builddir.Dir('libs').File('libtui.so')
        libfb = builddir.Dir('libs').File('libfb.so')
    else:
        libui = None
        libtui = None
        libfb = None

    libpedigree = builddir.File('libpedigree.so')
    libpedigree_c = builddir.File('libpedigree-c.so')

    # Build the disk images (whichever are the best choice for this system)
    forcemtools = env['forcemtools']
    buildImage = None
    if (not forcemtools) and env['distdir']:
        fileList.append(env['distdir'])

        buildImage = targetdir.buildImageTargetdir
    elif not forcemtools and (env['MKE2FS'] is not None):
        buildImage = debugfs.buildImageE2fsprogs
    elif not forcemtools and (env['LOSETUP'] is not None):
        fileList += ["#images/hdd_ext2.tar.gz"]

        buildImage = losetup.buildImageLosetup

    if forcemtools or buildImage is None:
        if env.File('#images/hdd_fat32.img').exists():
            fileList += ["#images/hdd_fat32.img"]
        else:
            fileList += ["#images/hdd_fat32.tar.gz"]

        buildImage = mtools.buildImageMtools

    # /boot directory
    if env['kernel_on_disk']:
        if 'STATIC_DRIVERS' in env['CPPDEFINES']:
            fileList += [kernel, config_database]
        else:
            fileList += [kernel, initrd, config_database]
            env.Depends(hddimg, 'initrd')

    # Add directories in the images directory.
    # TODO(miselin): fix this to use Dir/File
    for entry in os.listdir(imagedir.abspath):
        fileList += [os.path.join(imagedir.abspath, entry)]

    # Add applications that we build as part of the build process.
    if apps.exists():
        for app in apps.entries:
            try:
                fileList.append(apps.File(app))
            except TypeError:
                # entry is not a file
                pass
    else:
        print "Apps directory did not exist at time of build."
        print "'scons' will need to be run again to fully build the disk image."

    # Add modules, and drivers, that we build as part of the build process.
    if env['modules_on_disk']:
        if modules.exists():
            for module in modules.entries:
                if not (module.endswith('.o') or module.endswith('.o.debug')):
                    continue
                fileList += [modules.File(module)]

        if drivers.exists():
            for driver in drivers.entries:
                if not (driver.endswith('.o') or driver.endswith('.o.debug')):
                    continue
                fileList += [drivers.File(driver)]

    # Add libraries
    fileList += image_c_libs + [
        libpedigree_c,
        libui,
        libtui,
        libfb,
    ]

    if env['ARCH_TARGET'] != 'ARM':
        fileList.append(libpedigree)

    if env['ARCH_TARGET'] in ['X86', 'X64'] and not env['posix_musl']:
        fileList += [builddir.File('libSDL.so')]

    fileList = [x for x in fileList if x]
    env.Command(hddimg, fileList, SCons.Action.Action(buildImage, None))

    # Build the live CD ISO
    if env['iso']:
        env.Command(cdimg, [config_database, initrd, kernel, hddimg],
            SCons.Action.Action(livecd.buildCdImage, None))
        post.postImageBuild(cdimg, env, iso=True)

    if not env['distdir']:
        post.postImageBuild(hddimg, env)
