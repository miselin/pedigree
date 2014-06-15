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

####################################
# SCons build system for Pedigree
## Tyler Kennedy (AKA Linuxhq AKA TkTech)
####################################

import os
import shutil
import subprocess
import commands
Import(['env'])

# Subsystems always get built first
subdirs = [
    'src/subsys/posix',
    'src/subsys/pedigree-c'
]

# Currently the native API is only supported on x86 architectures
if env['ARCH_TARGET'] == 'X86' or env['ARCH_TARGET'] == 'X64':
    subdirs += ['src/subsys/native']

# Then modules and the system proper get built
subdirs += ['src/modules', 'src/system/kernel']

# On X86, X64 and PPC we build applications and LGPL libraries
if env['ARCH_TARGET'] in ['X86', 'X64', 'PPC']:
    subdirs += ['src/user', 'src/lgpl']
if not env['ARCH_TARGET'] in ['X86', 'X64']:
    subdirs += ['src/system/boot']

SConscript([os.path.join(i, 'SConscript') for i in subdirs],exports = ['env'])

rootdir = env.Dir("#").abspath
builddir = env.Dir("#" + env["PEDIGREE_BUILD_BASE"]).abspath
imagesroot = env.Dir("#images").abspath
imagedir = env.Dir(env['PEDIGREE_IMAGES_DIR']).abspath

# Build the configuration database (no dependencies)
configdb = env.File(builddir + '/config.db')
config_file = env.File('#src/modules/system/config/config_database.h')

# Complete the build - image creation
floppyimg = env.File(builddir + '/floppy.img')
hddimg = env.File(builddir + '/hdd.img')
cdimg = env.File(builddir + '/pedigree.iso')

additional_images = {}
if env['haveqemuimg'] and (env['createvdi'] or env['createvmdk']):
    additional_images = {'vdi': env.File(builddir + '/hdd.vdi'), 'vmdk' : env.File(builddir + '/hdd.vmdk')}

configSchemas = []
for i in os.walk(env.Dir("#src").abspath):
    configSchemas += map(lambda x: i[0] + '/' + x, filter(lambda y: y == 'schema', i[2]))
env.Command(configdb, configSchemas, '@cd ' + rootdir + ' && python ./scripts/buildDb.py')

def find_files(startdir, matcher, skip_paths):
    x = []
    for root, dirs, files in os.walk(startdir):
        ok = True
        for path in skip_paths:
            if path in root:
                ok = False
                break

        if ok:
            x.extend([os.path.join(root, f) for f in files if matcher(f)])
    return x

if env['pyflakes'] or env['sconspyflakes']:
    # Run pyflakes over .py files, if pyflakes is present.
    pyflakespath = commands.getoutput("which pyflakes")
    if os.path.exists(pyflakespath):
        def pyflakes_command(base, files):
            for pyfile in files:
                name = os.path.basename(pyfile)
                pyflakes = env.Command(
                    '%s-%s' % (base, name),
                    [],
                    '%s %s' % (pyflakespath, pyfile))
                env.Alias(base, '%s-%s' % (base, name))
                env.AlwaysBuild(pyflakes)

        if env['pyflakes']:
            # Find .py files in the tree, excluding the images directory.
            pyfiles = find_files(rootdir, lambda x: x.endswith('.py'), [imagesroot])
            pyflakes_command('pyflakes', pyfiles)

        # SConscripts are magically run with stuff already imported, so they
        # will never pass a pyflakes check. :(
        '''
        if env['sconspyflakes']:
            # Find SConstruct and all SConscripts.
            pyfiles = find_files(rootdir, lambda x: x in ['SConstruct', 'SConscript'], [imagesroot])
            pyflakes_command('sconspyflakes', pyfiles)
        '''

def makeHeader(target, source, env):
    f = open(target[0].path, "w")
    f.write("/* Made from " + source[0].path + " */\n")
    f.write("/* Autogenerated by the build system, do not edit. */\n")
    
    f.write("static uint8_t file[] = {\n");
    i = open(source[0].path, "rb")
    data = i.read()
    for i in data:
        f.write("0x%02x,\n" % (ord(i)))
    f.write("};\n")
    f.close()
    
if('STATIC_DRIVERS' in env['CPPDEFINES']):
    env.Command(config_file, configdb, makeHeader)

# TODO: If any of these commands fail, they WILL NOT STOP the build!

def postImageBuild(img, env):
    if env['haveqemuimg']:
        if env['createvdi'] and (additional_images.get('vdi') <> None):
            os.system("echo Creating hdd.vdi... && qemu-img convert -O vpc %s %s" % (img, additional_images.get('vdi').path))
        if env['createvmdk'] and (additional_images.get('vmdk') <> None):
            os.system("echo Creating hdd.vmdk... && qemu-img convert -f raw -O vmdk %s %s" % (img, additional_images.get('vmdk').path))

def buildImageTargetdir(target, source, env):
    if env['verbose']:
        print '      Copying to ' + os.path.basename(source[0].abspath)
    else:
        print '      Copying to \033[32m' + os.path.basename(source[0].abspath) + '\033[0m'

    builddir = env.Dir("#" + env["PEDIGREE_BUILD_BASE"]).abspath
    imagedir = env.Dir(env['PEDIGREE_IMAGES_DIR']).abspath
    appsdir = env.Dir(env['PEDIGREE_BUILD_APPS']).abspath
    modsdir = env.Dir(env['PEDIGREE_BUILD_MODULES']).abspath
    drvsdir = env.Dir(env['PEDIGREE_BUILD_DRIVERS']).abspath
    libsdir = os.path.join(builddir, 'libs')

    outFile = target[0].path
    targetDir = source[0].path
    source = source[1:]

    # Perhaps the menu.lst should refer to .pedigree-root :)
    os.system("cp -u " + builddir + "/config.db %s/.pedigree-root" % (targetDir,))
    os.system("mkdir -p %s/boot/grub" % (targetDir,))
    os.system("cp -u " + imagedir + "/../grub/menu-hdd.lst %s/boot/grub/menu.lst" % (targetDir,))

    # Copy the kernel, initrd, and configuration database
    for i in source[0:3]:
        os.system("cp -u " + i.abspath + " %s/boot/" % (targetDir,))
    source = source[3:]

    # Create needed directories for missing layout
    os.system("mkdir -p %s/system/modules" % (targetDir,))

    # Copy each input file across
    for i in source:
        otherPath = ''
        search, prefix = imagedir, ''

        # Applications
        if appsdir in i.abspath:
            search = appsdir
            prefix = '/applications'

        # Modules
        elif modsdir in i.abspath:
            search = modsdir
            prefix = '/system/modules'

        # Drivers
        elif drvsdir in i.abspath:
            search = drvsdir
            prefix = '/system/modules'

        # User Libraries
        elif libsdir in i.abspath:
            search = libsdir
            prefix = '/libraries'

        # Additional Libraries
        elif builddir in i.abspath:
            search = builddir
            prefix = '/libraries'

        otherPath = prefix + i.abspath.replace(search, '')

        # Clean out the last directory name if needed
        if(os.path.isdir(i.abspath)):
            otherPath = '/'.join(otherPath.split('/')[:-1])
            if(len(otherPath) == 0 or otherPath[0] != '/'):
                otherPath = '/' + otherPath

        os.system("cp -u -R " + i.path + " %s" % (targetDir,) + otherPath)

    os.system("mkdir -p %s/tmp" % (targetDir,))
    os.system("mkdir -p %s/config" % (targetDir,))
    os.system("cp -u " + imagedir + "/../base/config/greeting %s/config/greeting" % (targetDir,))
    os.system("cp -Ru " + imagedir + "/../base/config/term %s/config/term" % (targetDir,))
    os.system("cp -u " + imagedir + "/../base/config/inputrc %s/config/inputrc" % (targetDir,))
    os.system("cp -u " + imagedir + "/../base/.bashrc %s/.bashrc" % (targetDir,))
    os.system("cp -u " + imagedir + "/../base/.profile %s/.profile" % (targetDir,))

    if os.path.exists(outFile):
        os.unlink(outFile)
    with open(outFile, 'w') as f:
        pass

def buildImageLosetup(target, source, env):
    if env['verbose']:
        print '      Creating ' + os.path.basename(target[0].path)
    else:
        print '      Creating \033[32m' + os.path.basename(target[0].path) + '\033[0m'

    builddir = env.Dir("#" + env["PEDIGREE_BUILD_BASE"]).abspath
    imagedir = env.Dir(env['PEDIGREE_IMAGES_DIR']).abspath
    appsdir = env.Dir(env['PEDIGREE_BUILD_APPS']).abspath
    modsdir = env.Dir(env['PEDIGREE_BUILD_MODULES']).abspath
    drvsdir = env.Dir(env['PEDIGREE_BUILD_DRIVERS']).abspath
    libsdir = os.path.join(builddir, 'libs')

    outFile = target[0].path
    imageBase = source[0].path
    offset = 32256
    source = source[1:]

    # Copy the base image to the destination, overwriting any image that
    # may already exist there.
    if('gz' in imageBase):
        os.system("tar -xzf " + imageBase + " -C .")
        shutil.move(os.path.basename(imageBase).replace('tar.gz', 'img'), outFile)
    else:
        shutil.copy(imageBase, outFile)

    os.mkdir("tmp")
    os.system("sudo mount -o loop,rw,offset=" + str(offset) + " " + outFile + " ./tmp")

    # Perhaps the menu.lst should refer to .pedigree-root :)
    os.system("sudo cp " + builddir + "/config.db ./tmp/.pedigree-root")
    os.system("sudo cp " + imagedir + "/../grub/menu-hdd.lst ./tmp/boot/grub/menu.lst")

    # Copy the kernel, initrd, and configuration database
    for i in source[0:3]:
        os.system("sudo cp " + i.abspath + " ./tmp/boot/")
    source = source[3:]

    # Copy each input file across
    for i in source:
        otherPath = ''
        search, prefix = imagedir, ''

        # Applications
        if appsdir in i.abspath:
            search = appsdir
            prefix = '/applications'

        # Modules
        elif modsdir in i.abspath:
            search = modsdir
            prefix = '/system/modules'

        # Drivers
        elif drvsdir in i.abspath:
            search = drvsdir
            prefix = '/system/modules'

        # User Libraries
        elif libsdir in i.abspath:
            search = libsdir
            prefix = '/libraries'

        # Additional Libraries
        elif builddir in i.abspath:
            search = builddir
            prefix = '/libraries'

        otherPath = prefix + i.abspath.replace(search, '')

        # Clean out the last directory name if needed
        if(os.path.isdir(i.abspath)):
            otherPath = '/'.join(otherPath.split('/')[:-1])
            if(len(otherPath) == 0 or otherPath[0] != '/'):
                otherPath = '/' + otherPath

        os.system("sudo cp -R " + i.path + " ./tmp" + otherPath)

    os.system("sudo mkdir -p ./tmp/tmp")
    os.system("sudo mkdir -p ./tmp/config")
    os.system("sudo cp " + imagedir + "/../base/config/greeting ./tmp/config/greeting")
    os.system("sudo cp " + imagedir + "/../base/config/inputrc ./tmp/config/inputrc")
    os.system("sudo cp " + imagedir + "/../base/.bashrc ./tmp/.bashrc")
    os.system("sudo cp " + imagedir + "/../base/.profile ./tmp/.profile")
    os.system("sudo umount ./tmp")

    for i in os.listdir("tmp"):
        os.remove(i)
    os.rmdir("tmp")
    
    postImageBuild(outFile, env)

def buildImageMtools(target, source, env):
    if env['verbose']:
        print '      Creating ' + os.path.basename(target[0].path)
    else:
        print '      Creating \033[32m' + os.path.basename(target[0].path) + '\033[0m'

    builddir = env.Dir("#" + env["PEDIGREE_BUILD_BASE"]).abspath
    imagedir = env.Dir(env['PEDIGREE_IMAGES_DIR']).abspath
    appsdir = env.Dir(env['PEDIGREE_BUILD_APPS']).abspath
    modsdir = env.Dir(env['PEDIGREE_BUILD_MODULES']).abspath
    drvsdir = env.Dir(env['PEDIGREE_BUILD_DRIVERS']).abspath
    libsdir = os.path.join(builddir, 'libs')

    pathToGrub = env.Dir("#images/grub").abspath

    outFile = target[0].path
    imageBase = source[0].path
    source = source[1:]

    destDrive = "C:"

    execenv = os.environ.copy()
    execenv['MTOOLS_SKIP_CHECK'] = '1'

    def domkdir(name):
        args = [
            'mmd',
            '-Do',
            '%s%s' % (destDrive, name),
        ]

        subprocess.check_call(args, stdout=subprocess.PIPE, env=execenv)

    def docopy(source, dest):
        args = [
            'mcopy',
            '-Do',
        ]

        # Multiple sources to the same destination directory
        if isinstance(source, list):
            args.extend(source)

        # Recursively copy directories
        elif os.path.isdir(source):
            args.extend(['-bms', source])

        # Single, boring source.
        else:
            args.append(source)

        args.append('%s%s' % (destDrive, dest))

        # Some of these copies may fail due to missing symlinks etc
        subprocess.call(args, stdout=subprocess.PIPE, env=execenv)

    # Copy the base image to the destination, overwriting any image that
    # may already exist there.
    if('gz' in imageBase):
        args = ['tar', '-xzf', imageBase, '-C', os.path.dirname(outFile)]
        result = subprocess.call(args)
        if result != 0:
            return result

        outbasename = os.path.basename(outFile)
        actualbasename = os.path.basename(imageBase).replace('tar.gz', 'img')

        # Caveat where build/ is on a different filesystem (why would you do this)
        if outbasename != actualbasename:
            os.rename(
                os.path.join(builddir, actualbasename),
                outFile
            )
    else:
        shutil.copy(imageBase, outFile)

    # Calculate the full set of operations we need to do, before running commands.

    mkdirops = [
        '/config',
        '/system',
        '/system/modules',
    ]

    copyops = [
        ([x.abspath for x in source[0:3]], '/boot'),
        (os.path.join(builddir, 'config.db'), '/.pedigree-root'),
        (os.path.join(pathToGrub, 'menu-hdd.lst'), '/boot/grub/menu.lst'),
        (os.path.join(imagedir, '..', 'base', 'config', 'greeting'), '/config/greeting'),
        (os.path.join(imagedir, '..', 'base', 'config', 'inputrc'), '/config/inputrc'),
        (os.path.join(imagedir, '..', 'base', '.bashrc'), '/.bashrc'),
        (os.path.join(imagedir, '..', 'base', '.profile'), '/.profile'),
    ]

    for i in source[3:]:
        otherPath = ''
        search, prefix = imagedir, ''

        # Applications
        if appsdir in i.abspath:
            search = appsdir
            prefix = '/applications'

        # Modules
        elif modsdir in i.abspath:
            search = modsdir
            prefix = '/system/modules'

        # Drivers
        elif drvsdir in i.abspath:
            search = drvsdir
            prefix = '/system/modules'

        # User Libraries
        elif libsdir in i.abspath:
            search = libsdir
            prefix = '/libraries'

        # Additional Libraries
        elif builddir in i.abspath:
            search = builddir
            prefix = '/libraries'

        otherPath = prefix + i.abspath.replace(search, '')

        dirname = os.path.dirname(otherPath)
        if not dirname.endswith('/'):
            dirname += '/'

        copyops.append((i.abspath, dirname))

    try:
        # Open for use in mtools
        mtsetup = env.File("#/scripts/mtsetup.sh").abspath
        subprocess.check_call([mtsetup, '-h', outFile, "1"], stdout=subprocess.PIPE)

        for name in mkdirops:
            # print "mkdir %s" % (name,)
            domkdir(name)

        for src, dst in copyops:
            # print "cp %s -> %s" % (src, dst)
            docopy(src, dst)
    except subprocess.CalledProcessError as e:
        os.unlink(outFile)
        return e.returncode
    else:
        postImageBuild(outFile, env)

    return 0

def buildCdImage(target, source, env):
    if env['verbose']:
        print '      Creating ' + os.path.basename(target[0].path)
    else:
        print '      Creating \033[32m' + os.path.basename(target[0].path) + '\033[0m'

    builddir = env.Dir("#" + env["PEDIGREE_BUILD_BASE"]).abspath
    pathToGrub = env.Dir("#images/grub").abspath

    # Select correct stage2_eltorito for the target.
    stage2_eltorito = "stage2_eltorito-" + env['ARCH_TARGET'].lower()

    # mkisofs modifies stage2_eltorito to do its work.
    pathToStage2 = os.path.join(pathToGrub, stage2_eltorito)
    shutil.copy(pathToStage2, '%s.mkisofs' % (pathToStage2,))
    pathToStage2 += '.mkisofs'

    args = [
        env['isoprog'],
        '-D',
        '-joliet',
        '-graft-points',
        '-quiet',
        '-input-charset',
        'iso8859-1',
        '-R',
        '-b',
        'boot/grub/stage2_eltorito',
        '-no-emul-boot',
        '-boot-load-size',
        '4',
        '-boot-info-table',
        '-o',
        target[0].path,
        '-V',
        '"PEDIGREE"',
        'boot/grub/stage2_eltorito=%s' % (pathToStage2,),
        'boot/grub/menu.lst=%s' % (os.path.join(pathToGrub, 'menu.lst'),),
        'boot/kernel=%s' % (source[2].abspath,),
        'boot/initrd.tar=%s' % (source[1].abspath,),
        '/livedisk.img=%s' % (source[3].abspath,),
        '.pedigree-root=%s' % (source[0].abspath,),
    ]
    result = subprocess.call(args)

    os.unlink(pathToStage2)

    return result

if not env['ARCH_TARGET'] in ["X86", "X64", "PPC"]:
    print "No hard disk image being built, architecture doesn't need one."
#elif env["installer"]:
#    print "Oops, installer images aren't built yet. Tell pcmattman to write Python scripts"
#    print "to build these images, please."
elif (not env['nodiskimages']) or (env['distdir']):
    # Define dependencies
    env.Depends(hddimg, 'libs')
    env.Depends(hddimg, 'apps')
    env.Depends(hddimg, 'initrd')
    env.Depends(hddimg, configdb)
    if not env['nodiskimages'] and not env['noiso']:
        env.Depends(cdimg, hddimg) # Inherent dependency on libs/apps

    fileList = []

    kernel = os.path.join(builddir, 'kernel', 'kernel')
    initrd = os.path.join(builddir, 'initrd.tar')

    apps = os.path.join(builddir, 'apps')
    modules = os.path.join(builddir, 'modules')
    drivers = os.path.join(builddir, 'drivers')

    libc = os.path.join(builddir, 'libc.so')
    libm = os.path.join(builddir, 'libm.so')
    libload = os.path.join(builddir, 'libload.so')

    libpthread = os.path.join(builddir, 'libpthread.so')
    libpedigree = os.path.join(builddir, 'libpedigree.so')
    libpedigree_c = os.path.join(builddir, 'libpedigree-c.so')

    libui = os.path.join(builddir, 'libs', 'libui.so')

    # Build the disk images (whichever are the best choice for this system)
    if env['distdir']:
        fileList.append(env['distdir'])
        buildImage = buildImageTargetdir
    elif(env['havelosetup']):
        fileList += ["#/images/hdd_ext2.tar.gz"]
        buildImage = buildImageLosetup
    else:
        if env.File('#/images/hdd_fat32.img').exists():
            fileList += ["#/images/hdd_fat32.img"]
        else:
            fileList += ["#/images/hdd_fat32.tar.gz"]
        buildImage = buildImageMtools

    # /boot directory
    fileList += [kernel, initrd, configdb]

    # Add directories in the images directory.
    for entry in os.listdir(imagedir):
        fileList += [os.path.join(imagedir, entry)]

    # Add applications that we build as part of the build process.
    if os.path.exists(apps):
        for app in os.listdir(apps):
            fileList += [os.path.join(apps, app)]
    else:
        print "Apps directory did not exist at time of build."
        print "'scons' will need to be run again to fully build the disk image."

    # Add modules, and drivers, that we build as part of the build process.
    if os.path.exists(modules):
        for module in os.listdir(modules):
            fileList += [os.path.join(modules, module)]
    if os.path.exists(drivers):
        for driver in os.listdir(drivers):
            fileList += [os.path.join(drivers, driver)]

    # Add libraries
    fileList += [
        libc,
        libm,
        libload,
        libpthread,
        libpedigree,
        libpedigree_c,
        libui,
    ]

    if env['ARCH_TARGET'] in ['X86', 'X64']:
        fileList += [os.path.join(builddir, 'libSDL.so')]

    # Build the hard disk image
    env.Command(hddimg, fileList, Action(buildImage, None))

    # Build the live CD ISO
    if not env['noiso']:
        env.Command(
            cdimg,
            [configdb, initrd, kernel, hddimg],
            Action(buildCdImage, None))
