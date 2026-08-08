#!/usr/bin/env python3
"""
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
"""

import os
import sqlite3
import stat
import sys
import tarfile
import traceback
import urllib.error
import urllib.request

import pup_common

class TarEntry(object):
    def __init__(self, tar, info, name, linkname, symlink=False):
        self.tar = tar
        self.info = info
        self.name = name
        self.linkname = linkname
        self.symlink = symlink

        self.shortname = self.info.name
        self.shortlink = self.info.linkname

        # Make sure the caller can write to the created file always.
        self.mode = self.info.mode | stat.S_IWRITE

        if not self.linkname:
            self.data = self.tar.extractfile(self.info).read()

    def __repr__(self):
        extra = ''
        if self.linkname:
            extra = '%s target=%s' % (extra, self.shortlink)
        if self.symlink:
            extra = '%s (symlink)' % (extra,)
        return 'TarEntry %s%s' % (self.shortname, extra)

    def extract(self):
        if self.linkname:
            fn = os.link
            if self.symlink:
                fn = os.symlink

            try:
                if os.path.lexists(self.name):
                    os.unlink(self.name)
                fn(self.linkname, self.name)
            except OSError:
                print(
                    "Extracting %s failed, target %s does not exist."
                    % (self.shortname, self.shortlink)
                )
                return

            return

        with open(self.name, "wb") as f:
            f.write(self.data)

        os.chmod(self.name, self.mode)

def do_extract(arg):
    try:
        if isinstance(arg, list):
            for f in arg:
                f.extract()
        else:
            arg.extract()
    except Exception:
        print(traceback.format_exc())
        raise

def main(arglist):
    if not arglist:
        print("Usage: pup install <package> [config]", file=sys.stderr)
        return 2

    remotePath, localPath, installRoot, desiredArch = pup_common.getConfig(arglist[1:])

    if not os.path.exists(localPath):
        os.makedirs(localPath)
    if not os.path.exists(installRoot):
        os.makedirs(installRoot)
    
    s = sqlite3.connect(localPath + "/packages.pupdb")
    e = s.execute("select * from packages where name=? and arch=? order by ver desc limit 1", (arglist[0], desiredArch))
    data = e.fetchone()
    if data is None:
        s.close()
        
        print(
            "The package '%s' [%s] is not available. You may need to run "
            "`pup sync' to update the list of available packages."
            % (arglist[0], desiredArch)
        )
        return 1
    s.close()
    
    # Package name
    packageName = "%s-%s-%s" % (data[1], data[2], desiredArch)
    localFile = "%s/%s.pup" % (localPath, packageName)
    
    print("Preparing to install %s" % (packageName))
    
    if not os.path.exists(localFile):

        print("    -> Downloading...")

        for server in remotePath:
            remoteUrl = "%s/%s.pup" % (server, packageName)
        
            print("      + trying %s" % (remoteUrl), end="")
            try:
                urllib.request.urlretrieve(remoteUrl, localFile)
                print("(OK)")
                break
            except (OSError, urllib.error.URLError):
                print("(failed)")
                continue
        
        if not os.path.exists(localFile):
            print ("Error: couldn't download the package from any server. "
                   "Check your internet connection and try again.")
            return 1

    print("    -> Installing...")
    
    # TODO: track installed packages in a local database
    
    t = tarfile.open(localFile)

    # Perform extraction, but ignore owner/group information
    filelist = []
    for f in t.getmembers():
        name = os.path.join(installRoot, f.name)
        linkname = f.linkname
        if linkname:
            if not linkname.startswith('/'):
                base = installRoot
                if f.issym():
                    base = os.path.dirname(name)
                linkname = os.path.join(base, linkname)
            linkname = os.path.join(installRoot, linkname)
        mode = f.mode

        # Make sure the caller can write to the file always.
        mode |= stat.S_IWRITE

        # We need to create the file now.
        if f.isdir():
            if not os.path.exists(name):
                os.makedirs(name)
        elif f.isfile() or f.isreg() or f.issym() or f.islnk():
            obj = TarEntry(t, f, name, linkname, f.issym())
            filelist.append(obj)
        else:
            print("(%s is not a sane file type)" % (f.name,))

    # Make sure symlinks appear last in the list of files.
    files = [f for f in filelist if not f.linkname]
    links = [f for f in filelist if f.linkname]

    # TarFile shares one stream; extract sequentially so entry reads cannot
    # race each other or depend on multiprocessing pickling behaviour.
    do_extract(files)

    # Do links last and unthreaded, as they depend on extracted files.
    do_extract(links)
    
    print("Package %s [%s] is now installed." % (packageName, desiredArch))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
