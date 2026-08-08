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

import configparser
import os


def getConfig(args):
    # Check for a config file
    # TODO: proper option parsing.
    configFile = "/support/pup/pup.conf"
    if len(args) > 0:
        configFile = args[0]
    
    # TODO: error handling

    # Try and read the config file
    if os.path.exists(configFile):
        cp = configparser.ConfigParser()
        cp.read(configFile)

        localPath = cp.get("paths", "localdb")
        installRoot = cp.get("paths", "installroot")
        desiredArch = cp.get("settings", "arch")

        remotePath = [server[1] for server in cp.items("remotes")]
    else:
        # Sane defaults!
        localPath = "./local_repo"
        installRoot = "./install_root"
        remotePath = ["https://pup.pedigree-project.org"]
        desiredArch = "amd64"

    localPath = localPath.rstrip("/") or "/"
    installRoot = installRoot.rstrip("/") or "/"
    remotePath = [server.rstrip("/") for server in remotePath]

    return (remotePath, localPath, installRoot, desiredArch)
