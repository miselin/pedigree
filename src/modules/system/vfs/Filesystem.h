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

#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <processor/types.h>
#include <utilities/String.h>

class Disk;
class File;

/** This class provides the abstract skeleton that all filesystems must implement.
 *
 * Thanks to gr00ber at #osdev for the inspiration for the caching algorithms.
 */
class Filesystem
{
/** VFS can access nAliases */
    friend class VFS;

public:
    //
    // Public interface
    //

    /** Constructor - creates a blank object. */
    Filesystem();
    /** Destructor */
    virtual ~Filesystem() {}

    /** Populates this filesystem with data from the given Disk device.
     * \return true on success, false on failure. */
    virtual bool initialise(Disk *pDisk) =0;

    /** Type of the probing callback given to the VFS.
        Probe function - if this filesystem is found on the given Disk
        device, create a new instance of it and return that. Else return 0. */
    typedef Filesystem *(*ProbeCallback)(Disk *);

    /** Attempt to find a file or directory in this filesystem.
        \param path The path to the file, in UTF-8 format, without filesystem identifier
        (e.g. not "root:/file", but "/file").
        \param pStartNode The node to start parsing 'path' from - defaults to / but
        is expected to contain the current working directory.
        \return The file if one was found, or 0 otherwise or if there was an error.
    */
    virtual File *find(String path, File *pStartNode=0);

    /** Returns the root filesystem node. */
    virtual File* getRoot() =0;

    /** Returns a string identifying the volume label. */
    virtual String getVolumeLabel() =0;

    /** Creates a file on the filesystem - fails if the file's parent directory does not exist. */
    bool createFile(String path, uint32_t mask, File *pStartNode=0);

    /** Creates a directory on the filesystem. Fails if the dir's parent directory does not exist. */
    bool createDirectory(String path, File *pStartNode=0);

    /** Creates a symlink on the filesystem, with the given value. */
    bool createSymlink(String path, String value, File *pStartNode=0);

    /** Removes a file, directory or symlink.
        \note Will fail if it is a directory and is not empty. The failure mode
        is unspecified. */
    bool remove(String path, File *pStartNode=0);

    /** Returns the disk in use */
    Disk *getDisk()
    {
        return m_pDisk;
    }
    
    /** Is the filesystem readonly? */
    bool isReadOnly()
    {
        return m_bReadOnly;
    }

    /** Does the filesystem care about case sensitivity? */
    virtual bool isCaseSensitive()
    {
        return true;
    }

protected:
    /** createFile calls this after it has parsed the string path. */
    virtual bool createFile(File* parent, String filename, uint32_t mask) =0;
    /** createDirectory calls this after it has parsed the string path. */
    virtual bool createDirectory(File* parent, String filename) =0;
    /** createSymlink calls this after it has parsed the string path. */
    virtual bool createSymlink(File* parent, String filename, String value) =0;
    /** remove() calls this after it has parsed the string path. */
    virtual bool remove(File* parent, File* file) =0;
    /** is this entire filesystem read-only?  */
    bool m_bReadOnly;
    /** Disk device(if any). */
    Disk *m_pDisk;
private:

    /** Get the true root of the filesystem, considering potential jails. */
    File *getTrueRoot();

    /** Internal function to find a node - Returns 0 on failure or the node.
        \param pNode The node to start parsing 'path' from.
        \param path  The path from pNode to the destination node. */
    File *findNode(File *pNode, String path);

    /** Internal function to find a node's parent directory.
        \param path The path from pStartNode to the original file.
        \param pStartNode The node to start parsing 'path' from.
        \param[out] filename The child file's name. */
    File *findParent(String path, File *pStartNode, String &filename);

    /** Accessed by VFS */
    size_t m_nAliases;

    /** Copy constructor.
        \note NOT implemented. */
    Filesystem(const Filesystem&);
    /** Assignment operator.
        \note NOT implemented. */
    void operator=(const Filesystem &);
};

#endif
