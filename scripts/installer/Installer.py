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
import hashlib
import shlex
import shutil
import subprocess
import curses

from progressBar import progressBar


class InstallerException(Exception):
    pass


class Installer:

    def __init__(self, stdscr, package, filesdir="./files",
                 installdir="./install"):
        self._stdscr = stdscr
        self._filesdir = filesdir
        self._installdir = installdir
        self._packageName = package

        self._titlewin = None
        self._mainwin = None
        self._progwin = None
        self._statwin = None

        self._progBar = progressBar(0, 100, 56)

    def setupCurses(self):
        self._titlewin = self._stdscr.subwin(1, 80, 0, 0)
        self._mainwin = self._stdscr.subwin(23, 80, 1, 0)
        self._progwin = self._stdscr.subwin(10, 60, 6, 10)
        self._statwin = self._stdscr.subwin(1, 80, 24, 0)

        curses.init_pair(1, curses.COLOR_BLACK, curses.COLOR_WHITE)
        curses.init_pair(2, curses.COLOR_WHITE, curses.COLOR_CYAN)
        curses.init_pair(3, curses.COLOR_YELLOW, curses.COLOR_WHITE)
        curses.init_pair(4, curses.COLOR_RED, curses.COLOR_WHITE)

        self._titlewin.bkgd(' ', curses.color_pair(1))
        self._statwin.bkgd(' ', curses.color_pair(1))
        self._mainwin.bkgd(' ', curses.color_pair(2))

        self._titlewin.addstr(0, 0, 'Installing ' + self._packageName)
        self._statwin.addstr(0, 0, '')

        self.resetProgWin()

        self._stdscr.refresh()

    def resetProgWin(self):
        self._progwin.clear()
        self._progwin.bkgd(' ', curses.color_pair(1))
        self._progwin.box()

        self._stdscr.move(24, 79)

    def statusUpdate(self, msg):
        self._statwin.clear()
        self._statwin.addstr(0, 1, msg)
        self._statwin.refresh()

    def drawAlert(self, msg, title, colour_pair):
        msgLines = msg.rstrip().split('\n')

        height = len(msgLines) + 4
        errwin = self._mainwin.subwin(height, 50, (24 / 2) - (height / 2), 15)
        errwin.overlay(self._progwin)
        errwin.clear()
        errwin.bkgd(' ', curses.color_pair(1))
        errwin.box()
        errwin.addstr(0, 2, ' %s ' % title, curses.color_pair(colour_pair))

        self.statusUpdate('Press ENTER to acknowledge')

        y = 2
        for i in msgLines:
            if(len(i) > 50):
                firstPart = i[0:46]
                secondPart = i[46:]
                errwin.addstr(y, 2, firstPart)
                y += 1
                errwin.addstr(y, 2, secondPart)
            else:
                errwin.addstr(y, 2, i)
            y += 1

        errwin.refresh()

        # Wait for ENTER
        while 1:
            c = self._stdscr.getch()
            if(c == 13 or c == 10):
                break

        self._mainwin.clear()
        self._mainwin.refresh()
        self.resetProgWin()

    def drawError(self, msg, title='Error'):
        self.drawAlert(msg, title, 4)

    def drawWarning(self, msg, title='Warning'):
        self.drawAlert(msg, title, 3)

    def drawProgress(self, action, fileName, progress):
        self._progwin.addstr(1, 2, action + ', please wait...')
        self._progwin.addstr(3, 2, fileName)

        self._progBar.updateAmount(progress)
        self._progwin.addstr(5, 2, str(self._progBar))

        self._progwin.refresh()

        self.resetProgWin()

    def InstallerPage(self, msg):
        introwin = self._mainwin.subwin(20, 70, 3, 5)
        introwin.clear()
        introwin.box()
        introwin.bkgd(' ', curses.color_pair(1))

        msgLines = msg.split("\n")
        msgNum = len(msgLines)

        y = (20 / 2) - (msgNum / 2)
        for i in msgLines:
            introwin.addstr(y, (70 / 2) - (len(i) / 2), i)
            y += 1

        introwin.refresh()

        self.waitForKeyPress()

        self._mainwin.clear()
        self._mainwin.refresh()

    def introPage(self):
        msg = 'Welcome to the ' + self._packageName + ' installation!'
        msg += '\n\n\n'
        msg += 'The next steps will guide you through the installation of '
        msg += self._packageName + '.'
        msg += '\n\n'
        msg += 'Press ENTER to continue.'
        self.InstallerPage(msg)

    def done(self):
        self.statusUpdate('')
        msg = self._packageName + ' is now installed!'
        msg += '\n\n\n'
        msg += 'Remove the CD from the disk drive and press any key to reboot.'
        self.InstallerPage(msg)

    def selectDest(self):
        pass

    def installFiles(self):
        # Open the manifest.
        try:
            with open(os.path.join(self._filesdir, 'filelist.txt')) as f:
                lines = f.readlines()
        except:
            # Pass it up to the caller
            self.drawError('Couldn\'t open file list for reading.')
            raise

        self.statusUpdate('Copying files...')

        # Start copying files
        for i, line in enumerate(lines):
            # Remove trailing whitespace and split on spaces
            # File format:
            # <source path> <dest path> <md5> <compulsory>
            line = line.rstrip()
            if not line:
                continue

            row = line.split(' ')
            if len(row) != 4:
                self.drawError('Bad set in file list:\n%s\nThis set only has '
                               '%d entries!' % (line, len(row)))
                continue

            source_rel_path, dest_rel_path, md5, needed = row

            if needed.lower() == 'yes':
                needed = True
            else:
                needed = False

            source_path = os.path.join(self._filesdir, source_rel_path)
            target_path = os.path.join(self._installdir, dest_rel_path)

            dest_dir = os.path.dirname(target_path)
            if not os.path.exists(dest_dir):
                os.makedirs(dest_dir)

            progress = (i / float(len(lines))) * 100.0
            self.drawProgress('Copying files', target_path, progress)

            # Some files are meant to be empty, but present.
            if not source_rel_path:
                with open(target_path, 'w'):
                    pass
                continue

            # Copy the file.
            shutil.copy(source_path, target_path)

            # MD5 the newly copied file
            with open(target_path) as f:
                file_hash = hashlib.md5(f.read()).hexdigest()

            # Ensure the MD5 matches
            if file_hash != md5:
                if needed:
                    self.drawError('Compulsory file failed verification:'
                                   '\n%s' % target_path)
                    raise InstallerException('file failed verification')
                else:
                    self.drawWarning('File %d failed verification, continuing '
                                     'anyway:\n%s' % (i, target_path))

        self.statusUpdate('Copy complete.')

    def postInstall(self):
        self.statusUpdate('Please wait...')

        # Files copied, run post-install scripts now
        try:
            with open(os.path.join(self._filesdir, 'postinstall.txt')) as f:
                lines = f.readlines()
                for i, entry in enumerate(lines):
                    entry = entry.strip()
                    if not entry:
                        continue

                    self.drawProgress('Running script', entry,
                                      (i / float(len(lines))) * 100.0)

                    try:
                        subprocess.check_call(shlex.split(entry))
                    except:
                        self.drawWarning('Post-install script "%s" failed, '
                                         'continuing...' % entry)
        except:
            self.statusUpdate('Post-install scripts complete.')

    def waitForKeyPress(self):
        self._stdscr.getch()
