#!/usr/bin/env python2.7
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

import difflib
import re
import sys
import os


# Extra prefixes to add to headers to try and find them (some moved).
# First entry: prefix to add to include paths
# Second entry: prefix to add before the above prefix to help find headers
# Third entry: an additional prefix to add, but not use for finding headers
HEADER_SEARCH_PREFIXES = (
    ('', '', ''),
    ('modules', '', ''),
    ('modules/system', '', ''),
    ('subsys', '', ''),
    ('pedigree', 'subsys/native/include', ''),  # for e.g. native/ -> pedigree/native/
    ('', 'subsys/native/include/pedigree/native', 'pedigree/native'),  # for bare native headers
    ('debugger', 'system/include/pedigree/kernel', 'pedigree/kernel'),
    ('debugger/commands', 'system/include/pedigree/kernel', 'pedigree/kernel'),
    ('debugger/libudis86', 'system/include/pedigree/kernel', 'pedigree/kernel'),
    ('core', 'system/include/pedigree/kernel', 'pedigree/kernel'),
    ('', 'system/include/pedigree/kernel', 'pedigree/kernel'),
)

NEVER_REWRITE = (
    'stddef.h',
    'stdint.h',
    'stdarg.h',
)

VERBOSE = False
SHOW_DIFFS = False


def identify_path(path, extra_prefix, extra_include_prefix):
    if extra_prefix:
        inner = '%s/%s' % (extra_prefix, path)
    else:
        inner = path

    if extra_include_prefix:
        new_header = '%s/%s' % (extra_include_prefix, inner)
    else:
        new_header = inner

    return new_header


def process(sourcepath, headersdir):
    with open(sourcepath) as f:
        source = f.read()

    newdata = []
    for l in source.splitlines():
        orig_l = l.rstrip('\r\n')
        l = l.strip()

        # TODO: need to handle spaces between # and include...
        if not l.startswith('#include'):
            newdata.append(orig_l)
            continue

        l = re.sub('[ \t]+', ' ', l)
        # Handles #include"header.h" properly.
        l = l[len('#include'):].strip()

        fields = l.split(' ', 1)

        path = fields[0]
        path = path.strip('<">')

        if len(fields) > 1:
            extras = ' ' + fields[1]
        else:
            extras = ''

        if path in NEVER_REWRITE:
            newdata.append(orig_l)
            continue

        if path.startswith('/'):
            # Absolute path, ignore
            newdata.append(orig_l)
            continue

        for prefix, extra_search, include_prefix in HEADER_SEARCH_PREFIXES:
            target_path = os.path.join(headersdir, extra_search, prefix, path)
            if VERBOSE:
                print 'attempt: %s -> %s' % (path, target_path)
            if os.path.exists(target_path):
                if VERBOSE:
                    print 'success: %s rewrites %s -> %s' % (
                        path, path, identify_path(path, prefix, include_prefix))
                extra_prefix = prefix
                extra_include_prefix = include_prefix
                break
        else:
            # Same directory include, as a fallback?
            if os.path.exists(os.path.join(os.path.dirname(sourcepath), path)):
                if VERBOSE:
                    print 'local: %s' % (path,)
                newdata.append(orig_l)
                continue

            if VERBOSE:
                print 'failed: %s is probably not a kernel header' % (path,)
            newdata.append(orig_l)
            continue

        # Does exist, we need to rewrite.
        new_header = identify_path(path, prefix, include_prefix)
        newdata.append('#include "%s"%s' % (new_header, extras))

    if SHOW_DIFFS:
        new = newdata
        old = source.splitlines()

        for field in difflib.unified_diff(old, new, sourcepath, sourcepath, lineterm=''):
            print field

    if newdata == [l.strip('\r\n') for l in source.splitlines()]:
        return  # No changes need to be made.

    with open(sourcepath, 'w') as f:
        f.write('\n'.join(newdata))

        # Persist trailing newline if possible.
        if source[-1] == '\n':
            f.write('\n')


def main():
    if len(sys.argv) != 3:
        print >>sys.stderr, 'Usage: fixincludes.py [source directory] [headers directory]'
        exit(1)

    if not (os.path.isdir(sys.argv[1]) and os.path.isdir(sys.argv[2])):
        print >>sys.stderr, 'Two directories must be specified.'
        exit(1)

    # Traverse file tree.
    for (root, dirs, files) in os.walk(sys.argv[1]):
        for f in files:
            fl = f.lower()
            if not (fl.endswith('.cc') or f.endswith('.c') or f.endswith('.h')):
                if fl not in ('new',):
                    continue

            if f != 'main.cc':
                continue

            path = os.path.join(root, f)
            if VERBOSE:
                print 'Fixing includes for "%s"...' % (path,)
            process(path, sys.argv[2])


if __name__ == '__main__':
    main()
