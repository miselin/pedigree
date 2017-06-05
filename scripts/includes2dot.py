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

import collections
import hashlib
import os
import re
import sys


def process(sourcepath, nodes, relationships):
    with open(sourcepath) as f:
        source = f.read()

    sourcedir = os.path.dirname(sourcepath)

    # Need to use a hash for the filenames to avoid emitting an invalid
    # dot graph -- labels are used for visual representation.
    if sourcepath not in nodes:
        sourcehash = hashlib.sha256(sourcepath).hexdigest()
        nodes[sourcepath] = sourcehash

    for l in source.splitlines():
        orig_l = l.rstrip('\r\n')
        l = l.strip()

        # TODO: need to handle spaces between # and include...
        if not l.startswith('#include'):
            continue

        l = re.sub('[ \t]+', ' ', l)
        # Handles #include"header.h" properly.
        l = l[len('#include'):].strip()

        fields = l.split(' ', 2)

        path = fields[0]
        path = path.strip('<">')

        # Remove './', '../' and normalize path.
        if path.startswith('./'):
            path = os.path.join(sourcedir, path[2:])
        elif path.startswith('../'):
            path = os.path.join(os.path.dirname(sourcedir), path[3:])

        if path not in nodes:
            pathhash = hashlib.sha256(path).hexdigest()
            nodes[path] = pathhash

        relationships[path].append(sourcepath)


def main():
    if len(sys.argv) < 3:
        print >>sys.stderr, 'Usage: includes2dot.py [threshold] [output file] [source directories]'
        exit(1)

    try:
        threshold = int(sys.argv[1])
        if threshold < 0:
            raise ValueError()
    except ValueError:
        print >>sys.stderr, 'Threshold must be an integer larger than or equal to zero.'
        exit(1)

    # Traverse file tree.
    nodes = {}
    relationships = collections.defaultdict(list)
    for search_path in sys.argv[3:]:
        if not os.path.isdir(search_path):
            print >>sys.stderr, '%s is not a directory.' % (search_path,)
            exit(1)

        for (root, dirs, files) in os.walk(search_path):
            for f in files:
                path = os.path.join(root, f)
                print 'Parsing includes for "%s"...' % (path,)
                process(path, nodes, relationships)

    ok_nodes = set()
    with open(sys.argv[2], 'w') as f:
        f.write('digraph headerdeps {\n')
        f.write('graph [splines=true, overlap=false];\n')

        for header, sources in relationships.iteritems():
            headerhash = nodes[header]
            if len(sources) < threshold:
                continue

            for source in sources:
                left = nodes[source]
                right = headerhash
                ok_nodes.add(left)
                ok_nodes.add(right)
                f.write('n%s -> n%s;\n' % (left, right))

        for path, nodename in nodes.iteritems():
            if nodename in ok_nodes:
                f.write('n%s [label="%s"];\n' % (nodename, path))

        f.write('labelloc="t";\nlabel="Headers included by >=%d files.";\n' % (threshold,))
        f.write('\n}')

if __name__ == '__main__':
    main()
