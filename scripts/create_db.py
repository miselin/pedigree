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
import re
import subprocess
import sys
import tempfile


def main():
    """Generates a sqlite3 DB from the given .sql files."""
    all_sql = ''
    for filename in sys.argv[2:]:
        with open(filename, 'rb') as f:
            all_sql += f.read().decode('utf-8')

    tables = ''
    m = re.findall('^create table .*?;$', all_sql, re.M | re.S | re.I)
    for match in m:
        tables += match + '\n'

    all_sql = re.sub('create table .*?;', '', all_sql, flags=re.M | re.S | re.I)

    if os.path.isfile(sys.argv[1]):
        os.unlink(sys.argv[1])

    with tempfile.NamedTemporaryFile() as f:
        f.write(b'begin;')
        f.write(tables.encode('utf-8'))
        f.write(all_sql.encode('utf-8'))
        f.write(b'commit;')
        f.flush()

        f.seek(0)

        subprocess.check_call('sqlite3 %s' % (sys.argv[1],), stdin=f, shell=True)


if __name__ == '__main__':
    main()
