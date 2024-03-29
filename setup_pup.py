#!/usr/bin/env python3
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

# This script is designed to configure a pup config file with the paths from
# this checkout of Pedigree.

import os
import sys

try:
  from ConfigParser import SafeConfigParser
except ImportError:
  from configparser import SafeConfigParser


def main():

    scriptdir = os.path.dirname(os.path.realpath(__file__))

    pupConfigDefault = "%s/scripts/pup/pup.conf.default" % (scriptdir,)
    pupConfig = "%s/scripts/pup/pup.conf" % (scriptdir,)

    target_arch = 'i686'
    if len(sys.argv) > 1:
        target_arch = sys.argv[1]

    cfg = SafeConfigParser()
    cfg.read(pupConfigDefault)

    cfg.set('paths', 'installroot', '%s/images/local' % scriptdir)
    cfg.set('paths', 'localdb', '%s/images/local/support/pup/db' % scriptdir)

    cfg.set('settings', 'arch', target_arch)

    with open(pupConfig, 'w') as f:
        cfg.write(f)

    print("Configuration file '%s' updated." % (pupConfig))


if __name__ == '__main__':
    main()

