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

import random
import select
import socket
import string


s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('192.168.15.2', 8080))

r = random.SystemRandom()


def build_message(length):
    msg = ''
    while len(msg) < length:
        msg += string.ascii_letters

    msg = msg[:length]

    return msg

print 'Building messages!'

# 1024 1 MiB messages.
msgs = []
for i in range(1):
    msgs.append(build_message(0x100000))
    # msgs.append(build_message(0x10000))

print 'Transmitting messages!'

for msg in msgs:
    s.send(msg)

# Finished.
s.send('\x01')

print 'All messages are sent, receiving replies!'

s.setblocking(0)

block = ''
while True:
    r, _, _ = select.select([s], [], [], 30)
    if s in r:
        msg = s.recv(4096)
        if msg:
            block += msg
    else:
        break

    if '\x01' in block:
        break

print 'Checking!'

check = ''.join(msgs) + '\x01'

if block != check:
    print 'NOPE'
    print len(block)
    print 'vs'
    print len(check)
else:
    print 'YEP'

s.close()
