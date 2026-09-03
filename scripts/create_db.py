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

import argparse
import os
import re
import subprocess
import tempfile

try:
    from shutil import which as find_executable
except ImportError:
    from distutils.spawn import find_executable


def main():
    """Generates a sqlite3 DB from the given .sql files."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--sqlite',
        default='auto',
        help='sqlite3 executable to use, or embedded to use Python sqlite3',
    )
    parser.add_argument('output')
    parser.add_argument('schemas', nargs='+')
    arguments = parser.parse_args()

    all_sql = ''
    for filename in arguments.schemas:
        with open(filename, 'rb') as f:
            all_sql += f.read().decode('utf-8')

    tables = ''
    m = re.findall('^create table .*?;$', all_sql, re.M | re.S | re.I)
    for match in m:
        tables += match + '\n'

    all_sql = re.sub('create table .*?;', '', all_sql, flags=re.M | re.S | re.I)

    if os.path.isfile(arguments.output):
        os.unlink(arguments.output)

    sql = 'begin;' + tables + all_sql + 'commit;'
    sqlite_executable = arguments.sqlite
    if sqlite_executable == 'auto':
        sqlite_executable = find_executable('sqlite3')

    if sqlite_executable and sqlite_executable != 'embedded':
        with tempfile.TemporaryFile() as script:
            script.write(sql.encode('utf-8'))
            script.seek(0)
            subprocess.check_call([sqlite_executable, arguments.output], stdin=script)
        return

    import sqlite3

    connection = sqlite3.connect(arguments.output)
    try:
        connection.executescript(sql)
    finally:
        connection.close()


if __name__ == '__main__':
    main()
