/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "pedigree/kernel/Log.h"
#include "modules/system/ext2/Ext2Filesystem.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/String.h"
#include "modules/system/vfs/Directory.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"

#include "DiskImage.h"

#ifdef HAVE_OPENSSL
#include <openssl/sha.h>
#endif

#define FS_ALIAS "fs"
#define TO_FS_PATH(x) String(FS_ALIAS "»") += x.c_str()

static bool ignoreErrors = false;
static size_t blocksPerRead = 64;

static uint32_t defaultPermissions[3] = {
    // File - RW-R--R--
    0644,
    // Directory, RWX-R-XR-X
    0755,
    // Symlink, RWXRWXRWX
    0777,
};

static size_t defaultOwner[2] = {
    // root:root
    0, 0,
};

enum CommandType
{
    InvalidCommand,
    CreateDirectory,
    CreateSymlink,
    CreateHardlink,
    WriteFile,
    RemoveFile,
    VerifyFile,
    ChangePermissions,
    ChangeOwner,
    SetDefaultPermissions,
    SetDefaultOwners,
};

struct Command
{
    Command() : what(InvalidCommand), params(){};

    CommandType what;
    std::vector<std::string> params;
    std::string original;
};

extern bool msdosProbeDisk(Disk *pDisk);
extern bool appleProbeDisk(Disk *pDisk);

class StreamingStderrLogger : public Log::LogCallback
{
  public:
    /// printString is used directly as well as in this callback object,
    /// therefore we simply redirect to it.
    void callback(const char *str)
    {
        fprintf(stderr, "%s", str);
    }
};

uint32_t getUnixTimestamp()
{
    return time(0);
}

static uint32_t modeToPermissions(uint32_t mode)
{
    uint32_t permissions = 0;
    if (mode & S_IRUSR)
        permissions |= FILE_UR;
    if (mode & S_IWUSR)
        permissions |= FILE_UW;
    if (mode & S_IXUSR)
        permissions |= FILE_UX;
    if (mode & S_IRGRP)
        permissions |= FILE_GR;
    if (mode & S_IWGRP)
        permissions |= FILE_GW;
    if (mode & S_IXGRP)
        permissions |= FILE_GX;
    if (mode & S_IROTH)
        permissions |= FILE_OR;
    if (mode & S_IWOTH)
        permissions |= FILE_OW;
    if (mode & S_IXOTH)
        permissions |= FILE_OX;
    return permissions;
}

bool writeFile(const std::string &source, const std::string &dest)
{
    struct stat st;
    if (stat(source.c_str(), &st) < 0)
    {
        std::cerr << "Could not open source file '" << source
                  << "': " << strerror(errno) << "." << std::endl;
        return false;
    }

    std::ifstream ifs(source, std::ios::binary);
    if (ifs.bad() || ifs.fail())
    {
        std::cerr << "Could not open source file '" << source << "'."
                  << std::endl;
        return false;
    }

    bool result =
        VFS::instance().createFile(TO_FS_PATH(dest), defaultPermissions[0]);
    if (!result)
    {
        std::cerr << "Could not create destination file '" << dest << "'."
                  << std::endl;
        return false;
    }

    File *pFile = VFS::instance().find(TO_FS_PATH(dest));
    if (!pFile)
    {
        std::cerr << "Couldn't open created destination file: '" << dest << "'."
                  << std::endl;
        return false;
    }

    // Do file block allocation now instead of during write()s below.
    pFile->preallocate(st.st_size);

    size_t blockSize = pFile->getBlockSize() * blocksPerRead;

    char *buffer = new char[blockSize];

    uint64_t offset = 0;
    while (!ifs.eof())
    {
        ifs.read(buffer, blockSize);
        uint64_t readCount = ifs.gcount();
        uint64_t count = 0;
        if (readCount)
        {
            count = pFile->write(
                offset, readCount, reinterpret_cast<uintptr_t>(buffer));
            if (!count || (count < readCount))
            {
                std::cerr << "Empty or short write to file '" << dest << "'."
                          << std::endl;
                if (!ignoreErrors)
                    return false;
            }

            offset += readCount;
        }
    }

    delete[] buffer;

    pFile->setUid(defaultOwner[0]);
    pFile->setGid(defaultOwner[1]);

    return true;
}

bool createSymlink(const std::string &name, const std::string &target)
{
    bool result =
        VFS::instance().createSymlink(TO_FS_PATH(name), String(target.c_str()));
    if (!result)
    {
        std::cerr << "Could not create symlink '" << name << "' -> '" << target
                  << "'." << std::endl;
        return false;
    }

    File *pResult = VFS::instance().find(TO_FS_PATH(name));
    if (pResult)
    {
        pResult->setPermissions(modeToPermissions(defaultPermissions[2]));
        pResult->setUid(defaultOwner[0]);
        pResult->setGid(defaultOwner[1]);
    }

    return true;
}

bool createHardlink(const std::string &name, const std::string &target)
{
    File *pTarget = VFS::instance().find(TO_FS_PATH(target));
    if (!pTarget)
    {
        std::cerr << "Couldn't open hard link target file: '" << target << "'."
                  << std::endl;
        return false;
    }

    bool result = VFS::instance().createLink(TO_FS_PATH(name), pTarget);
    if (!result)
    {
        std::cerr << "Could not create hard link '" << name << "' -> '"
                  << target << "'." << std::endl;
        return false;
    }

    return true;
}

bool createDirectory(const std::string &dest)
{
    bool result = VFS::instance().createDirectory(
        TO_FS_PATH(dest), defaultPermissions[1]);
    if (!result)
    {
        std::cerr << "Could not create directory '" << dest << "'."
                  << std::endl;
        return false;
    }

    File *pResult = VFS::instance().find(TO_FS_PATH(dest));
    if (pResult)
    {
        pResult->setUid(defaultOwner[0]);
        pResult->setGid(defaultOwner[1]);
    }

    return true;
}

bool removeFile(const std::string &target)
{
    bool result = VFS::instance().remove(TO_FS_PATH(target));
    if (!result)
    {
        std::cerr << "Could not remove file '" << target << "'." << std::endl;
        return false;
    }

    return true;
}

bool verifyFile(const std::string &source, const std::string &target)
{
    std::ifstream ifs(source, std::ios::binary);
    if (ifs.bad() || ifs.fail())
    {
        std::cerr << "Could not open verify source file '" << source << "'."
                  << std::endl;
        return false;
    }

    File *pFile = VFS::instance().find(TO_FS_PATH(target));
    if (!pFile)
    {
        std::cerr << "Couldn't open verify target file: '" << target << "'."
                  << std::endl;
        return false;
    }

    // Don't use the cache for this - read blocks directly via the FS driver.
    pFile->enableDirect();

    // Compare block by block.
    size_t blockSize = pFile->getBlockSize() * blocksPerRead;
    char *bufferA = new char[blockSize];
    char *bufferB = new char[blockSize];

    uint64_t offset = 0;
    while (!ifs.eof())
    {
        ifs.read(bufferA, blockSize);
        uint64_t readCount = ifs.gcount();
        uint64_t count = 0;
        if (readCount)
        {
            count = pFile->read(
                offset, blockSize, reinterpret_cast<uintptr_t>(bufferB));
            if (!count || (count < readCount))
            {
                std::cerr << "Empty or short read from file '" << target << "'."
                          << std::endl;
                if (!ignoreErrors)
                    return false;
            }

            if (memcmp(bufferA, bufferB, count) != 0)
            {
                std::cerr << "Files do not match at block starting at offset "
                          << offset << "." << std::endl;
                for (size_t i = 0; i < count; ++i)
                {
                    if (bufferA[i] == bufferB[i])
                        continue;
                    std::cerr << "First difference at offset " << offset + i
                              << ": ";
                    std::cerr << (int) bufferA[i] << " vs " << (int) bufferB[i]
                              << std::endl;
                    break;
                }
                if (!ignoreErrors)
                    return false;
            }

            offset += readCount;
        }
    }

    delete[] bufferA;
    delete[] bufferB;

    return true;
}

bool changePermissions(
    const std::string &filename, const std::string &permissions)
{
    int intPerms = 0;
    try
    {
        intPerms = std::stoi(permissions, nullptr, 8);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Bad permissions value '" << permissions
                  << "' passed: " << e.what() << std::endl;
        return false;
    }

    File *pFile = VFS::instance().find(TO_FS_PATH(filename));
    if (!pFile)
    {
        std::cerr << "Couldn't open file to change permissions: '" << filename
                  << "'." << std::endl;
        return false;
    }

    pFile->setPermissions(modeToPermissions(intPerms));

    return true;
}

bool changeOwner(
    const std::string &filename, const std::string &uid, const std::string &gid)
{
    int intUid = 0, intGid = 0;
    try
    {
        intUid = std::stoi(uid, nullptr);
        intGid = std::stoi(uid, nullptr);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Bad uid/gid value '" << uid << "' or '" << gid
                  << "' passed: " << e.what() << std::endl;
        return false;
    }

    File *pFile = VFS::instance().find(TO_FS_PATH(filename));
    if (!pFile)
    {
        std::cerr << "Couldn't open file to change permissions: '" << filename
                  << "'." << std::endl;
        return false;
    }

    pFile->setUid(intUid);
    pFile->setGid(intGid);

    return true;
}

bool setDefaultPermissions(
    const std::string &file_perms, const std::string &dir_perms,
    const std::string &link_perms)
{
    int intFile = 0, intDir = 0, intLink = 0;
    try
    {
        intFile = std::stoi(file_perms, nullptr, 8);
        intDir = std::stoi(dir_perms, nullptr, 8);
        intLink = std::stoi(link_perms, nullptr, 8);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Bad default permissions passed: " << e.what()
                  << std::endl;
        return false;
    }

    defaultPermissions[0] = intFile;
    defaultPermissions[1] = intDir;
    defaultPermissions[2] = intLink;

    return true;
}

bool setDefaultOwner(const std::string &uid, const std::string &gid)
{
    int intUid = 0, intGid = 0;
    try
    {
        intUid = std::stoi(uid, nullptr);
        intGid = std::stoi(gid, nullptr);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Bad uid/gid value '" << uid << "' or '" << gid
                  << "' passed for defaults: " << e.what() << std::endl;
        return false;
    }

    defaultOwner[0] = intUid;
    defaultOwner[1] = intGid;

    return true;
}

bool probeAndMount(const char *image, size_t part)
{
    // Prepare to probe ext2 filesystems via the VFS.
    VFS::instance().addProbeCallback(&Ext2Filesystem::probe);

    static DiskImage mainImage(image);
    if (!mainImage.initialise())
    {
        std::cerr << "Couldn't load disk image!" << std::endl;
        return false;
    }

    bool isFullFilesystem = false;
    if (!msdosProbeDisk(&mainImage))
    {
        std::cerr << "No MSDOS partition table found, trying an Apple "
                     "partition table."
                  << std::endl;
        if (!appleProbeDisk(&mainImage))
        {
            std::cerr << "No partition table found, assuming this is an ext2 "
                         "filesystem."
                      << std::endl;
            isFullFilesystem = true;
        }
    }

    size_t desiredPartition = part;

    Disk *pDisk = 0;

    if (isFullFilesystem)
        pDisk = &mainImage;
    else
    {
        // Find the nth partition.
        if (desiredPartition > mainImage.getNumChildren())
        {
            std::cerr << "Desired partition does not exist in this image."
                      << std::endl;
            return false;
        }

        pDisk = static_cast<Disk *>(mainImage.getChild(desiredPartition));
    }

    // Make sure we actually have a filesystem here.
    String alias("fs");
    if (!VFS::instance().mount(pDisk, alias))
    {
        std::cerr << "This partition does not appear to be an ext2 filesystem."
                  << std::endl;
        return false;
    }

    return true;
}

#ifdef HAVE_OPENSSL
void checksumFile(File *pFile)
{
    uint8_t hash[SHA256_DIGEST_LENGTH];

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    size_t blockSize = pFile->getBlockSize() * blocksPerRead;
    uint8_t *buffer = new uint8_t[blockSize];

    size_t offset = 0;
    while (offset < pFile->getSize())
    {
        // Read the file and hash it.
        size_t numBytes =
            pFile->read(offset, blockSize, reinterpret_cast<uintptr_t>(buffer));
        if (!numBytes)
        {
            break;
        }

        SHA256_Update(&ctx, buffer, numBytes);

        if (numBytes < blockSize)
        {
            break;
        }

        offset += numBytes;
    }

    delete[] buffer;

    SHA256_Final(hash, &ctx);

    std::cout << pFile->getFullPath() << ": ";
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i)
    {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(hash[i]);
    }
    std::cout << std::endl;
}
#endif

int imageChecksums(const char *image, size_t part = 0)
{
    if (!probeAndMount(image, part))
    {
        return 1;
    }

#ifdef HAVE_OPENSSL

    std::vector<File *> files;
    files.push_back(VFS::instance().find(TO_FS_PATH(std::string("/"))));

    size_t n = 0;

    for (auto it = files.begin(); it != files.end(); ++it, ++n)
    {
        File *pFile = *it;
        if (pFile->isDirectory())
        {
            Directory *pDirectory = Directory::fromFile(pFile);
            for (size_t i = 0; i < pDirectory->getNumChildren(); ++i)
            {
                File *pChild = pDirectory->getChild(i);
                if (pChild->getName() == String(".") ||
                    pChild->getName() == String(".."))
                {
                    continue;
                }

                files.push_back(pChild);
            }

            // Pushing to the vector invalidates our iterator, so renew it.
            it = files.begin() + n;

            continue;
        }
        else if (pFile->isSymlink())
        {
            continue;
        }

        checksumFile(pFile);
    }
#else
    std::cerr << "ext2img was built without any support for sha256."
              << std::endl;
#endif

    return 0;
}

int handleImage(
    const char *image, std::vector<Command> &cmdlist, size_t part = 0)
{
    if (!probeAndMount(image, part))
    {
        return 1;
    }

    // Handle the command list.
    size_t nth = 0;
    for (auto it = cmdlist.begin(); it != cmdlist.end(); ++it, ++nth)
    {
        switch (it->what)
        {
            case WriteFile:
                if ((!writeFile(it->params[0], it->params[1])) && !ignoreErrors)
                {
                    return 1;
                }
                break;
            case CreateSymlink:
                if ((!createSymlink(it->params[0], it->params[1])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case CreateHardlink:
                if ((!createHardlink(it->params[0], it->params[1])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case CreateDirectory:
                if ((!createDirectory(it->params[0])) && !ignoreErrors)
                {
                    return 1;
                }
                break;
            case RemoveFile:
                if ((!removeFile(it->params[0])) && !ignoreErrors)
                {
                    return 1;
                }
                break;
            case VerifyFile:
                if ((!verifyFile(it->params[0], it->params[1])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case ChangePermissions:
                if ((!changePermissions(it->params[0], it->params[1])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case ChangeOwner:
                if ((!changeOwner(
                        it->params[0], it->params[1], it->params[2])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case SetDefaultPermissions:
                if ((!setDefaultPermissions(
                        it->params[0], it->params[1], it->params[2])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            case SetDefaultOwners:
                if ((!setDefaultOwner(it->params[0], it->params[1])) &&
                    !ignoreErrors)
                {
                    return 1;
                }
                break;
            default:
                std::cerr << "Unknown command in command list." << std::endl;
                break;
        }

        if ((nth % 10) == 0)
        {
            double progress = nth / (double) cmdlist.size();
            std::cout << "Progress: " << std::setprecision(4)
                      << (progress * 100.0) << "%      \r" << std::flush;
        }
    }

    std::cout << "\rProgress: 100.0%" << std::endl;

    std::cout << "Completed command list for image " << image << "."
              << std::endl;

    return 0;
}

bool parseCommandFile(const char *cmdFile, std::vector<Command> &output)
{
    std::ifstream f(cmdFile);
    if (f.bad() || f.fail())
    {
        std::cerr << "Command file '" << cmdFile << "' could not be read."
                  << std::endl;
        return false;
    }

    std::string line;
    size_t lineno = 0;
    while (std::getline(f, line))
    {
        ++lineno;

        if (line.empty())
        {
            continue;
        }

        if (line[0] == '#')
        {
            // Comment line.
            continue;
        }

        Command c;
        std::istringstream s(line);

        std::string cmd;
        s >> cmd;

        std::string next;
        while (s >> next)
        {
            if (next.empty())
            {
                continue;
            }

            c.params.push_back(next);
        }

        size_t requiredParamCount = 0;

        bool ok = true;

        if (cmd == "write")
        {
            c.what = WriteFile;
            requiredParamCount = 2;
        }
        else if (cmd == "symlink")
        {
            c.what = CreateSymlink;
            requiredParamCount = 2;
        }
        else if (cmd == "hardlink")
        {
            c.what = CreateHardlink;
            requiredParamCount = 2;
        }
        else if (cmd == "mkdir")
        {
            c.what = CreateDirectory;
            requiredParamCount = 1;
        }
        else if (cmd == "rm")
        {
            c.what = RemoveFile;
            requiredParamCount = 1;
        }
        else if (cmd == "verify")
        {
            c.what = VerifyFile;
            requiredParamCount = 2;
        }
        else if (cmd == "chmod")
        {
            c.what = ChangePermissions;
            requiredParamCount = 2;
        }
        else if (cmd == "chown")
        {
            c.what = ChangeOwner;
            requiredParamCount = 3;
        }
        else if (cmd == "defaultperms")
        {
            c.what = SetDefaultPermissions;
            requiredParamCount = 3;
        }
        else if (cmd == "defaultowner")
        {
            c.what = SetDefaultOwners;
            requiredParamCount = 2;
        }
        else
        {
            std::cerr << "Unknown command '" << cmd << "' at line " << lineno
                      << ": '" << line << "'" << std::endl;
            ok = false;
        }

        if (c.params.size() < requiredParamCount)
        {
            std::cerr << "Not enough parameters for '" << cmd << "' at line "
                      << lineno << ": '" << line << "'" << std::endl;
            ok = false;
        }

        if (!ok)
        {
            if (!ignoreErrors)
                return false;

            continue;
        }

        c.original = line;
        output.push_back(c);
    }

    return true;
}

int main(int argc, char *argv[])
{
    const char *cmdFile = 0;
    const char *diskImage = 0;
    size_t partitionNumber = 0;
    bool quiet = false;
    bool sums = false;

    // Load options.
    int c;
    while ((c = getopt(argc, argv, "qif:c:p::b:s")) != -1)
    {
        switch (c)
        {
            case 'c':
                // cmdfile
                cmdFile = optarg;
                break;
            case 'f':
                // disk image
                diskImage = optarg;
                break;
            case 'i':
                ignoreErrors = true;
                break;
            case 'q':
                quiet = true;
                break;
            case 'p':
                // partition number
                partitionNumber = atoi(optarg);
                break;
            case 'b':
                // # of blocks per read.
                blocksPerRead = atoi(optarg);
                break;
            case 's':
                // Do checksums of every file on the disk instead of running a
                // list.
                sums = true;
                break;
            case '?':
                if (optopt == 'c' || optopt == 'p')
                {
                    std::cerr << "Option -" << optopt
                              << " requires an argument." << std::endl;
                }
                else
                {
                    std::cerr << "Option -" << optopt << " is unknown."
                              << std::endl;
                }
                break;
            default:
                return 1;
        }
    }

    if (sums && cmdFile)
    {
        std::cerr << "Checksums cannot be performed with a command list."
                  << std::endl;
        return 1;
    }

    if (!sums && (cmdFile == 0))
    {
        std::cerr << "A command file must be specified." << std::endl;
        return 1;
    }

    if (diskImage == 0)
    {
        std::cerr << "A disk image must be specified." << std::endl;
        return 1;
    }

    // Enable logging.
    StreamingStderrLogger logger;
    if (!quiet)
    {
        Log::instance().installCallback(&logger, true);
    }

    int rc = 1;
    if (sums)
    {
        if (imageChecksums(diskImage, partitionNumber))
        {
            rc = 0;
        }
    }
    else
    {
        // Parse!
        std::vector<Command> cmdlist;
        if (parseCommandFile(cmdFile, cmdlist))
        {
            // Complete tasks.
            rc = handleImage(diskImage, cmdlist, partitionNumber);
        }
    }
    if (!quiet)
    {
        Log::instance().removeCallback(&logger);
    }
    std::cout << std::flush;
    return rc;
}
