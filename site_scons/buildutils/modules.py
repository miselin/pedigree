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

import buildutils.misc


def buildModule(env, stripped_target, target, sources, depends=(), shtarget=None):
    module_env = env.Clone()

    kernel_dir = env['PEDIGREE_BUILD_KERNEL']
    kernel_so = kernel_dir.File('libkernel.so')

    if env['clang_cross']:
        module_env['LINKFLAGS'] = env['CLANG_BASE_LINKFLAGS']
    else:
        # Wipe out the kernel's link flags so we can substitute our own.
        module_env['LINKFLAGS'] = []

    for key in ('CFLAGS', 'CCFLAGS', 'CXXFLAGS'):
        module_env[key] = module_env['TARGET_%s' % key]

    if "STATIC_DRIVERS" in env['CPPDEFINES']:
        module_env['LSCRIPT'] = module_env.File("#src/modules/link_static.ld")
    else:
        module_env['LSCRIPT'] = module_env.File("#src/modules/link.ld")

    extra_linkflags = module_env.get('MODULE_LINKFLAGS', [])

    module_env.MergeFlags({
        'LINKFLAGS': ['-nodefaultlibs', '-nostartfiles', '-Wl,-T,$LSCRIPT',
                      '-Wl,-shared'] + extra_linkflags,
    })

    if env['lto']:
        module_env.MergeFlags({
            'CCFLAGS': ['-flto'],
            'LINKFLAGS': ['-flto'],
        })

    libmodule_dir = module_env['BUILDDIR'].Dir('modules')
    libsubsys_dir = module_env['BUILDDIR'].Dir('subsys')
    libmodule_path = libmodule_dir.File('libmodule.a')

    depend_libs = []
    if shtarget:
        for entry in depends:
            depend_libs.append('%s.so' % (entry,))

    buildutils.misc.removeFromAllFlags(module_env, ['-mcmodel=kernel'])

    module_env.MergeFlags({
        'CCFLAGS': ['-fPIC', '-fno-omit-frame-pointer'],
        'LIBS': ['module', 'gcc', kernel_so] + depend_libs,
        'LIBPATH': [libmodule_dir, libsubsys_dir.glob('*')],
        'CPPDEFINES': ['IN_PEDIGREE_KERNEL'],  # modules are in-kernel
    })

    if shtarget:
        # No need for lto in the shared object build as it's only used for
        # verifying needed symbols are present before runtime
        shared_module_env = module_env.Clone()
        buildutils.misc.removeFromAllFlags(shared_module_env, ['-flto'])
        shared_module = shared_module_env.SharedLibrary(shtarget, sources)

    module_env.Depends(target, libmodule_path)
    module_env.Depends(stripped_target, libmodule_path)
    if shtarget:
        module_env.Depends(target, shared_module)

    if env['clang_cross'] and env['clang_analyse']:
        return module_env.Program(stripped_target, sources)

    intermediate = module_env.Program(target, sources)
    module_env.Depends(target, module_env['LSCRIPT'])
    module_env.Depends(target, libmodule_path)
    return module_env.Command(stripped_target, intermediate,
                              action='$STRIP -d --strip-debug -o $TARGET '
                                     '$SOURCE')
