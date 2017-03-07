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

import multiprocessing
import os
import re
import select
import signal
import socket
import sys
import subprocess
import time
import Queue


class JobException(Exception):
    pass


class TimeoutError(Exception):
    pass


class Timeout(object):
    def __init__(self, seconds=1):
        self.seconds = seconds

    def handle_timeout(self, signum, frame):
        raise TimeoutError('timed out')

    def __enter__(self):
        signal.signal(signal.SIGALRM, self.handle_timeout)
        signal.alarm(self.seconds)

    def __exit__(self, type, value, traceback):
        signal.alarm(0)

    def cancel(self):
        # No need to get the alarm anymore.
        signal.signal(signal.SIGALRM, signal.SIG_IGN)


def job(script, args, env, timeout, regex, out_queue):
    # Open UDP socket for the serial port.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock_qemu = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Get ports we can use
    # We need to do this this way so we can run multiple jobs concurrently.
    sock.bind(('127.0.0.1', 0))
    sock_qemu.bind(('127.0.0.1', 0))
    port_me = sock.getsockname()[1]
    port_qemu = sock_qemu.getsockname()[1]

    # Set up two-way connectivity for the UDP socket.
    sock.connect(('127.0.0.1', port_qemu))

    args = args[:]
    args.extend([
        # for us to read/write the serial port and drive the debugger
        '-serial', 'udp:127.0.0.1:%d@:%d' % (port_me, port_qemu),
    ])

    sock_qemu.close()
    qemu = subprocess.Popen([script] + args, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            close_fds=True, env=env)

    # Skip GRUB menu.
    time.sleep(1)
    try:
        qemu.stdin.write('sendkey ret\n')
    except:
        print qemu.communicate()
        exit(1)

    found = False
    serial = ''
    try:
        with Timeout(timeout) as t:
            terminating = False
            while True:
                reads, _, excepts = select.select([sock], [], [sock], 2)
                if sock in reads:
                    msg = sock.recv(1024)
                    if msg:
                        serial += msg
                elif sock in excepts:
                    print 'Serial port socket hit an exceptional condition.'
                    print 'Aborting test.'
                    raise JobException()
                elif terminating:
                    print ('QEMU terminated before seeing Pedigree boot to '
                           'the login prompt.')
                    print 'This may indicate a triple fault.'
                    raise JobException()

                if regex.search(serial):
                    t.cancel()
                    print 'Regex matched, collecting data and terminating.'
                    for command in (' ', 'dump\n', 'disassemble\n',
                                    'backtrace\n'):
                        sock.send(command)

                    time.sleep(1)
                    found = True
                    break

                elif ('<< Flushing log content >>' in serial or
                        'Press any key to enter the debugger...' in serial):
                    # Run the debugger on the serial port so we can see what
                    # happened after the fact.
                    for command in (' ', 'dump\n', 'disassemble\n',
                                    'backtrace\n'):
                        sock.send(command)

                    time.sleep(1)
                    found = False
                    break
                elif qemu.poll() is not None:
                    # qemu has terminated!
                    terminating = True

    except (TimeoutError, JobException):
        print 'Test timed/errored out without matching desired regex.'

    if qemu.poll() is None:
        stdout, stderr = qemu.communicate('quit\n')

    # Go non-blocking on the socket from now on.
    sock.setblocking(0)

    # Read whatever we can from the socket now.
    while True:
        try:
            msg = sock.recv(1024)
        except socket.error:
            break
        if not msg:
            break
        serial += msg
        if len(msg) < 1024:
            break

    sock.close()

    pid = os.getpid()
    if found:
        with open('debug_result.%d.txt' % pid, 'w') as f:
            f.write(serial)

    out_queue.put((found, pid))


def main():
    qemu_script = os.path.join(os.path.dirname(__file__), 'qemu')
    qemu_env = os.environ.copy()
    qemu_env['NO_SERIAL_PORTS'] = 'yes'

    extra_args = [
        '-monitor', 'stdio',
        '-no-reboot', '-nographic',
    ]

    if len(sys.argv) < 3:
        print >>sys.stderr, "Usage: debug_harness.py [timeout] [failure_regex]"
        return 1

    timeout = int(sys.argv[1])
    regex = sys.argv[2]

    r = re.compile(regex, re.MULTILINE)

    # Leave one CPU spare for this parent process to schedule onto.
    job_count = max(1, multiprocessing.cpu_count() - 1)

    keep_going = True
    while keep_going:
        print 'Starting %d QEMU instances to find matches for regex %r' % (
            job_count, regex)
        results = multiprocessing.Queue()
        jobs = []
        for i in range(job_count):
            proc = multiprocessing.Process(target=job, args=(
                qemu_script, extra_args, qemu_env, timeout, r, results))
            proc.start()
            jobs.append(proc)

        for j in jobs:
            j.join()

            while True:
                try:
                    v = results.get_nowait()
                except Queue.Empty:
                    break

                found, pid = v
                if found:
                    print 'Success! See debug_serial.%d.txt for the error log.' % pid
                    keep_going = False


if __name__ == '__main__':
    exit(main())
