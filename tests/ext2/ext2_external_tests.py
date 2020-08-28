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
import subprocess
import sys
import unittest


class Ext2ExternalTests(unittest.TestCase):

    def tearDown(self):
        # Clean up the data files now that we're done.
        # We make copies of each test file so we don't accidentally corrupt them
        # due to errors in ext2img.
        if os.path.exists('iut.img'):  # image-under-test
            os.unlink('iut.img')


def decompress(path, out):
    with open(out, 'wb') as f:
        subprocess.check_call(['gunzip', '-d', '-q', '-k', '-c', path], stdout=f)


def generate_new_test(testname, ext2img, src, should_pass=True):
    """Generate a test that runs ext2img to complete."""
    def _setup(self):
        # Pre-test: create the image.
        decompress(src, 'iut.img')

    def call(self, wrapper=None):
        try:
            _setup(self)
        except:
            self.skipTest('cannot create image for test, skipping')

        args = [ext2img, '-s', '-f', 'iut.img']
        if wrapper:
            args = wrapper + args
        result = subprocess.Popen(args, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)

        try:
            _, run_output = result.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            self.fail('ext2img timed out')
        run_result = result.returncode

        run_output = run_output.decode('utf-8')

        if should_pass:
            self.assertEqual(run_result, 0, 'exit status %d != 0\n'
                             'output:\n\n%s\n' % (
                                 run_result, run_output))
        else:
            self.assertNotEqual(run_result, 0, 'exit status %d == 0\n'
                                'ext2img output:\n%s\n' % (run_result,
                                                           run_output))

    def test_doer(self):
        call(self)

    def test_memcheck_doer(self):
        call(self, wrapper=['valgrind', '--tool=memcheck',
                            '--error-exitcode=1'])

    def test_sgcheck_doer(self):
        call(self, wrapper=['valgrind', '--tool=exp-sgcheck',
                            '--error-exitcode=1'])

    returns = (
        test_doer,
        #test_memcheck_doer,
        # test_sgcheck_doer,
    )

    for r in returns:
        r.__name__ = r.__name__.replace('doer', testname)

    return returns


def find_external_tests():
    """Find external tests to run.

    These are expected to come from an e2fsprogs checkout, in the 'tests'
    directory. That directory contains a suite of tests for the various
    e2fsprogs tools. We generally want to work with the ones with an 'f_'
    prefix as these are disk images in various states of disrepair.
    """
    if 'PEDIGREE_EXT2_EXTERNAL_DIR' not in os.environ:
        print('$PEDIGREE_EXT2_EXTERNAL_DIR must be set for external tests.')
        exit(0)

    # Should be run from the top level of the source tree.
    ext2img_bin = 'build-host/src/buildutil/ext2img'

    extdir = os.environ['PEDIGREE_EXT2_EXTERNAL_DIR']
    for f in os.listdir(extdir):
        p = os.path.join(extdir, f)
        if not os.path.isdir(p):
            continue

        # Only pull in tests
        if not f.startswith('f_'):
            continue

        # Only pull in tests with disk images
        diskimg = os.path.join(p, 'image.gz')
        if not os.path.isfile(diskimg):
            continue

        # Generate test
        tests = generate_new_test('test_%s' % (f,), ext2img_bin, diskimg)
        for test in tests:
            setattr(Ext2ExternalTests, test.__name__, test)

find_external_tests()
