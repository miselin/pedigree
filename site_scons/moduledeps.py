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


# Module interdependencies - all transitive dependencies will be included
module_dependencies = {
    '3c90x': ['network-stack'],
    'network-stack': ['lwip'],
    'ata': ['scsi'],
    'cdi': ['dma', 'network-stack'],
    'dm9601': ['usb', 'network-stack'],
    'e1000': ['cdi'],
    'ftdi': ['usb'],
    'ne2k': ['network-stack'],
    'pcnet': ['cdi'],
    'rtl8139': ['cdi'],
    'sis900': ['cdi'],
    'usb-hcd': ['usb'],
    'usb-hub': ['usb'],
    'usb-mass-storage': ['usb', 'scsi'],
    'usb-hid': ['usb', 'hid'],
    'vbe': ['config'],
    'splash': ['config'],
    'ext2': ['vfs'],
    'fat': ['vfs'],
    'iso9660': ['vfs'],
    'console': ['vfs'],
    'linker': ['vfs'],
    'users': ['config'],
    'ramfs': ['vfs'],
    'rawfs': ['vfs'],
    'lodisk': ['vfs'],
    'status_server': ['network-stack', 'lwip', 'vfs'],
    'preload': ['vfs'],
    'init': ['vfs', 'posix'],
    'confignics': ['network-stack', 'lwip'],
    'mountroot': ['vfs', 'ramfs', 'lodisk'],
    'pcap': ['network-stack'],
    'posix': ['linker', 'lwip', 'ps2mouse', 'console', 'ramfs', 'users'],  # TODO: need to skip ps2mouse depending on architecture!
    'pedigree-c': ['config', 'posix', 'vfs'],
    'native': [],
}


def expandDependencies(x):
    """Expand deps from the given list including all transitive deps."""
    deps = []
    for dep in x:
        mod_deps = module_dependencies.get(dep, [])
        deps.append(dep)
        deps.extend(expandDependencies(mod_deps))

    return deps
