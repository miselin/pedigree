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


class Ext2Tests(unittest.TestCase):

    def tearDown(self):
        # Clean up the data files now that we're done.
        if os.path.exists('_t.img'):
            os.unlink('_t.img')
        if os.path.exists('big.dat'):
            os.unlink('big.dat')
        if os.path.exists('_sparse.dat'):
            os.unlink('_sparse.dat')


def generate_new_test(ext2img, script, should_pass, sz=0x1000000, suffix=None,
                      blocksz=None, verifies=None, absents=None,
                      fixture=None):
    """Generate a test that runs ext2img to complete."""
    def _setup(self):
        # Pre-test: create the image.
        with open('_t.img', 'w') as f:
            f.truncate(sz)
        # Pre-test: create a big data file for large write testing.
        with open('big.dat', 'w') as f:
            with open('/dev/urandom', 'r') as f_:
                # Avoid tri-indirect addressing for now (not yet implemented).
                if sz * 0.7 < 0x1000000:
                    f.write('x' * int(sz * 0.7))
        args = ['mke2fs', '-q', '-O', '^dir_index', '-I', '128', '-F',
                '-L', 'pedigree']
        if blocksz is not None:
            args.extend(['-b', str(blocksz)])
        args.append('_t.img')
        subprocess.check_call(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        if fixture:
            def debugfs(command):
                result = subprocess.Popen(
                    ['debugfs', '-w', '-R', command, '_t.img'],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                output, _ = result.communicate()
                output = output.decode('utf-8')
                self.assertEqual(
                    result.returncode, 0,
                    'debugfs command failed: %s\n%s' % (command, output))
                return output

        if fixture == 'inline-symlink':
            debugfs('symlink /inline ABCD')
            inode = debugfs('stat /inline')
            self.assertRegex(inode, r'\bSize:\s+4\b')
            self.assertRegex(inode, r'\bBlockcount:\s+0\b')
        elif fixture == 'sparse-direct':
            with open('_sparse.dat', 'wb') as f:
                f.write(b'x' * (12 * 1024))

            debugfs('write _sparse.dat /sparse')
            debugfs('punch /sparse 1 1')
            inode = debugfs('stat /sparse')
            self.assertRegex(inode, r'\bSize:\s+12288\b')
            for block in (0, 2, 11):
                bmap = debugfs(
                    'bmap /sparse %d' % block).strip().splitlines()[-1]
                self.assertNotEqual(
                    bmap, '0',
                    'sparse fixture data block %d is a hole' % block)
            hole = debugfs('bmap /sparse 1').strip().splitlines()[-1]
            self.assertEqual(hole, '0', 'sparse fixture block 1 is allocated: %s' % hole)

    def call(self, wrapper=None):
        try:
            _setup(self)
        except subprocess.CalledProcessError:
            self.skipTest('cannot create image for test, skipping')

        args = [ext2img, '-c', script, '-f', '_t.img']
        if wrapper:
            args = wrapper + args
        result = subprocess.Popen(args, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)

        run_output, _ = result.communicate()
        run_result = result.returncode

        run_output = run_output.decode('utf-8')

        if should_pass:
            self.assertEqual(run_result, 0, 'exit status %d != 0\n'
                             'output:\n\n%s\n' % (
                                 run_result, run_output))

            # Make sure an fsck passes too, now that we've checked the
            # invocation itself passed.
            args = ['fsck.ext2', '-n', '-f', '_t.img']
            fsck = subprocess.Popen(args, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            fsck_result = fsck.wait()
            fsck_output, _ = fsck.communicate()
            self.assertEqual(fsck_result, 0,
                             msg='exit status %d != 0\nfsck output:\n\n%s\n'
                                 'ext2img output:\n%s\n' % (fsck_result,
                                                            fsck_output,
                                                            run_output))

            if verifies is not None:
                for imgfile, localfile in verifies:
                    # Pull out the file from the image for testing
                    args = ['debugfs', '-R', 'dump %s _f' % (imgfile,), '_t.img']
                    subprocess.check_call(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

                    # Do the files match?
                    args = ['cmp', localfile, '_f']
                    try:
                        subprocess.check_call(args)
                    except subprocess.CalledProcessError:
                        self.fail('The file "%s" in the image does not match with the local file "%s".' % (imgfile, localfile))

            if absents is not None:
                for imgfile in absents:
                    args = ['debugfs', '-R', 'stat %s' % (imgfile,), '_t.img']
                    result = subprocess.Popen(args, stdout=subprocess.PIPE,
                                              stderr=subprocess.STDOUT)
                    output, _ = result.communicate()
                    output = output.decode('utf-8')
                    self.assertIn(
                        'File not found', output,
                        'The file "%s" still exists in the image:\n%s' %
                        (imgfile, output))
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

    testname = os.path.basename(script).replace('.test', '').replace('.', '_')
    if suffix is not None:
        testname += '.%s' % (suffix,)

    returns = (
        test_doer,
        # test_memcheck_doer,
        # test_sgcheck_doer,
    )

    for r in returns:
        r.__name__ = r.__name__.replace('doer', testname)

    return returns


def find_pedigree_tests():
    # Should be run from the top level of the source tree.
    ext2img_bin = os.environ.get(
        'PEDIGREE_EXT2IMG', 'build-host/src/buildutil/ext2img')
    testdir = 'tests/ext2'

    # Find tests to run.
    for f in os.listdir(testdir):
        f = os.path.join(testdir, f)

        # Each .test file is a command list for ext2img. The first line of a
        # .test file needs to indicate (in a comment) whether this invocation
        # should succeed or fail.
        if not f.endswith('.test'):
            continue

        blocksz = None
        verifies = []
        absents = []
        fixture = None
        with open(f) as f_:
            header = f_.read(128).splitlines()
            start = header[0]
            should_pass = 'pass' in start.lower()

            # Allow temporary disables for tests
            if 'disable' in start.lower():
                continue

            # Test mode for big blocks
            if 'bigblocks' in start.lower():
                blocksz = 16384

            for line in header[1:]:
                if 'fixture:' in line.lower():
                    fixture = line.split(' ')[-1]
                elif 'blocksize:' in line.lower():
                    blocksz = int(line.split(' ')[-1])

            # Look for a verify
            try:
                nextline = header[1]
            except IndexError:
                continue

            if 'verify:' in nextline.lower():
                x = nextline.split(' ')
                verifies.append((x[-2], x[-1]))

            for line in header[1:]:
                if 'absent:' in line.lower():
                    absents.append(line.split(' ')[-1])

        for sz in (0x100000 * 16, 0x100000 * 256, 0x100000 * 512):
            tests = generate_new_test(ext2img_bin, f, should_pass, sz=sz,
                                      suffix='%dMB' % (sz / 0x100000,),
                                      blocksz=blocksz, verifies=verifies,
                                      absents=absents,
                                      fixture=fixture)
            for test in tests:
                setattr(Ext2Tests, test.__name__, test)


find_pedigree_tests()
