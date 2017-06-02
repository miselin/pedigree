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

#include "Ext2Node.h"
#include "Ext2Filesystem.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/assert.h"

Ext2Node::Ext2Node(uintptr_t inode_num, Inode *pInode, Ext2Filesystem *pFs)
    : m_pInode(pInode), m_InodeNumber(inode_num), m_pExt2Fs(pFs), m_Blocks(),
      m_nMetadataBlocks(0), m_nSize(LITTLE_TO_HOST32(pInode->i_size))
{
    // i_blocks == # of 512-byte blocks. Convert to FS block count.
    uint32_t blockCount = LITTLE_TO_HOST32(pInode->i_blocks);
    uint32_t totalBlocks = (blockCount * 512) / m_pExt2Fs->m_BlockSize;

    size_t dataBlockCount = m_nSize / m_pExt2Fs->m_BlockSize;
    if (m_nSize % m_pExt2Fs->m_BlockSize)
    {
        ++dataBlockCount;
    }

    m_Blocks.reserve(dataBlockCount, false);
    m_nMetadataBlocks = totalBlocks - dataBlockCount;

    for (size_t i = 0; i < 12 && i < dataBlockCount; i++)
        m_Blocks.pushBack(LITTLE_TO_HOST32(m_pInode->i_block[i]));

    // We'll read these later.
    for (size_t i = 12; i < dataBlockCount; ++i)
    {
        m_Blocks.pushBack(~0);
    }
}

Ext2Node::~Ext2Node()
{
}

uintptr_t Ext2Node::readBlock(uint64_t location)
{
    // Sanity check.
    uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
    if (nBlock > m_Blocks.count())
    {
        NOTICE("beyond blocks [" << nBlock << ", " << m_Blocks.count() << "]");
        return 0;
    }
    if (location > m_nSize)
    {
        NOTICE("beyond size [" << location << ", " << m_nSize << "]");
        return 0;
    }

    ensureBlockLoaded(nBlock);
    uintptr_t result = m_pExt2Fs->readBlock(m_Blocks[nBlock]);

    // Add any remaining offset we chopped off.
    result += location % m_pExt2Fs->m_BlockSize;
    return result;
}

void Ext2Node::writeBlock(uint64_t location)
{
    // Sanity check.
    uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
    if (nBlock > m_Blocks.count())
        return;
    if (location > m_nSize)
        return;

    // Update on disk.
    ensureBlockLoaded(nBlock);
    m_pExt2Fs->writeBlock(m_Blocks[nBlock]);
}

void Ext2Node::trackBlock(uint32_t block)
{
    m_Blocks.pushBack(block);

    // Inode i_blocks field is actually the count of 512-byte blocks.
    uint32_t i_blocks =
        ((m_Blocks.count() + m_nMetadataBlocks) * m_pExt2Fs->m_BlockSize) / 512;
    m_pInode->i_blocks = HOST_TO_LITTLE32(i_blocks);

    // Write updated inode.
    m_pExt2Fs->writeInode(getInodeNumber());
}

void Ext2Node::wipe()
{
    NOTICE(
        "wipe: " << m_Blocks.count() << " blocks, size is " << m_nSize
                 << "...");
    for (size_t i = 0; i < m_Blocks.count(); ++i)
    {
        ensureBlockLoaded(i);
        NOTICE("wipe: releasing block: " << Hex << m_Blocks[i]);
        m_pExt2Fs->releaseBlock(m_Blocks[i]);
    }
    m_Blocks.clear();

    m_nSize = 0;

    m_pInode->i_size = 0;
    m_pInode->i_blocks = 0;
    ByteSet(m_pInode->i_block, 0, sizeof(uint32_t) * 15);

    // Write updated inode.
    m_pExt2Fs->writeInode(getInodeNumber());
    NOTICE("wipe done");
}

void Ext2Node::extend(size_t newSize)
{
    ensureLargeEnough(newSize);
}

bool Ext2Node::ensureLargeEnough(size_t size, bool onlyBlocks)
{
    // The majority of times this is called, we won't need to allocate blocks.
    // So, we check for that early. Then, we can move on to actually allocating
    // blocks if that is necessary.
    size_t currentMaxSize = m_Blocks.count() * m_pExt2Fs->m_BlockSize;
    if (LIKELY(size <= currentMaxSize))
    {
        if (size > m_nSize && !onlyBlocks)
        {
            // preallocate() doesn't change this, so fix the mismatch now.
            m_nSize = size;
            fileAttributeChanged(
                m_nSize, LITTLE_TO_HOST32(m_pInode->i_atime),
                LITTLE_TO_HOST32(m_pInode->i_mtime),
                LITTLE_TO_HOST32(m_pInode->i_ctime));
        }
        return true;
    }
    else if (!onlyBlocks)
    {
        m_nSize = size;
        fileAttributeChanged(
            m_nSize, LITTLE_TO_HOST32(m_pInode->i_atime),
            LITTLE_TO_HOST32(m_pInode->i_mtime),
            LITTLE_TO_HOST32(m_pInode->i_ctime));
    }

    size_t delta = size - currentMaxSize;
    size_t deltaBlocks = delta / m_pExt2Fs->m_BlockSize;
    if (delta % m_pExt2Fs->m_BlockSize)
    {
        ++deltaBlocks;
    }

    // Allocate the needed blocks.
    Vector<uint32_t> newBlocks;
#if 1
    if (!m_pExt2Fs->findFreeBlocks(m_InodeNumber, deltaBlocks, newBlocks))
    {
        SYSCALL_ERROR(NoSpaceLeftOnDevice);
        return false;
    }
#else
    for (size_t i = 0; i < deltaBlocks; ++i)
    {
        uint32_t block = m_pExt2Fs->findFreeBlock(m_InodeNumber);
        if (!block)
        {
            SYSCALL_ERROR(NoSpaceLeftOnDevice);
            return false;
        }
        else
        {
            newBlocks.pushBack(block);
        }
    }
#endif

    for (auto block : newBlocks)
    {
        if (!addBlock(block))
        {
            ERROR("Adding block " << block << " failed!");
            return false;
        }

        // Load the block and zero it.
        uint8_t *pBuffer =
            reinterpret_cast<uint8_t *>(m_pExt2Fs->readBlock(block));
        ByteSet(pBuffer, 0, m_pExt2Fs->m_BlockSize);
    }

    return true;
}

bool Ext2Node::ensureBlockLoaded(size_t nBlock)
{
    if (nBlock > m_Blocks.count())
    {
        FATAL(
            "EXT2: ensureBlockLoaded: Algorithmic error [block "
            << nBlock << " > " << m_Blocks.count() << "].");
    }
    if (m_Blocks[nBlock] == ~0U)
        getBlockNumber(nBlock);

    return true;
}

bool Ext2Node::getBlockNumber(size_t nBlock)
{
    size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

    assert(nBlock >= 12);

    if (nBlock < nPerBlock + 12)
    {
        getBlockNumberIndirect(
            LITTLE_TO_HOST32(m_pInode->i_block[12]), 12, nBlock);
        return true;
    }

    if (nBlock < (nPerBlock * nPerBlock) + nPerBlock + 12)
    {
        getBlockNumberBiindirect(
            LITTLE_TO_HOST32(m_pInode->i_block[13]), nPerBlock + 12, nBlock);
        return true;
    }

    getBlockNumberTriindirect(
        LITTLE_TO_HOST32(m_pInode->i_block[14]),
        (nPerBlock * nPerBlock) + nPerBlock + 12, nBlock);

    return true;
}

bool Ext2Node::getBlockNumberIndirect(
    uint32_t inode_block, size_t nBlocks, size_t nBlock)
{
    uint32_t *buffer =
        reinterpret_cast<uint32_t *>(m_pExt2Fs->readBlock(inode_block));

    for (size_t i = 0;
         i < m_pExt2Fs->m_BlockSize / 4 && nBlocks < m_Blocks.count(); i++)
    {
        m_Blocks[nBlocks++] = LITTLE_TO_HOST32(buffer[i]);
    }

    return true;
}

bool Ext2Node::getBlockNumberBiindirect(
    uint32_t inode_block, size_t nBlocks, size_t nBlock)
{
    size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

    uint32_t *buffer =
        reinterpret_cast<uint32_t *>(m_pExt2Fs->readBlock(inode_block));

    // What indirect block does nBlock exist on?
    size_t nIndirectBlock = (nBlock - nBlocks) / nPerBlock;

    getBlockNumberIndirect(
        LITTLE_TO_HOST32(buffer[nIndirectBlock]),
        nBlocks + nIndirectBlock * nPerBlock, nBlock);

    return true;
}

bool Ext2Node::getBlockNumberTriindirect(
    uint32_t inode_block, size_t nBlocks, size_t nBlock)
{
    size_t nPerBlock = m_pExt2Fs->m_BlockSize / 4;

    uint32_t *buffer =
        reinterpret_cast<uint32_t *>(m_pExt2Fs->readBlock(inode_block));

    // What biindirect block does nBlock exist on?
    size_t nBiBlock = (nBlock - nBlocks) / (nPerBlock * nPerBlock);

    getBlockNumberBiindirect(
        LITTLE_TO_HOST32(buffer[nBiBlock]),
        nBlocks + nBiBlock * nPerBlock * nPerBlock, nBlock);

    return true;
}

bool Ext2Node::addBlock(uint32_t blockValue)
{
    size_t nEntriesPerBlock = m_pExt2Fs->m_BlockSize / 4;

    // Calculate whether direct, indirect or tri-indirect addressing is needed.
    if (m_Blocks.count() < 12)
    {
        // Direct addressing is possible.
        m_pInode->i_block[m_Blocks.count()] = HOST_TO_LITTLE32(blockValue);
    }
    else if (m_Blocks.count() < 12 + nEntriesPerBlock)
    {
        // Indirect addressing needed.
        size_t indirectIdx = m_Blocks.count() - 12;

        // If this is the first indirect block, we need to reserve a new table
        // block.
        if (m_Blocks.count() == 12)
        {
            uint32_t newBlock = m_pExt2Fs->findFreeBlock(m_InodeNumber);
            m_pInode->i_block[12] = HOST_TO_LITTLE32(newBlock);
            if (m_pInode->i_block[12] == 0)
            {
                // We had a problem.
                SYSCALL_ERROR(NoSpaceLeftOnDevice);
                return false;
            }

            void *buffer =
                reinterpret_cast<void *>(m_pExt2Fs->readBlock(newBlock));
            ByteSet(buffer, 0, m_pExt2Fs->m_BlockSize);

            // Write back the zeroed block to prepare the indirect block.
            m_pExt2Fs->writeBlock(newBlock);

            // Taken on a new block - update block count (but don't track in
            // m_Blocks, as this is a metadata block).
            m_nMetadataBlocks++;
        }

        // Now we can set the block.
        uint32_t bufferBlock = LITTLE_TO_HOST32(m_pInode->i_block[12]);
        uint32_t *buffer =
            reinterpret_cast<uint32_t *>(m_pExt2Fs->readBlock(bufferBlock));

        buffer[indirectIdx] = HOST_TO_LITTLE32(blockValue);
        m_pExt2Fs->writeBlock(bufferBlock);
    }
    else if (
        m_Blocks.count() <
        12 + nEntriesPerBlock + nEntriesPerBlock * nEntriesPerBlock)
    {
        // Bi-indirect addressing required.

        // Index from the start of the bi-indirect block (i.e. ignore the 12
        // direct entries and one indirect block).
        size_t biIdx = m_Blocks.count() - 12 - nEntriesPerBlock;
        // Block number inside the bi-indirect table of where to find the
        // indirect block table.
        size_t indirectBlock = biIdx / nEntriesPerBlock;
        // Index inside the indirect block table.
        size_t indirectIdx = biIdx % nEntriesPerBlock;

        // If this is the first bi-indirect block, we need to reserve a
        // bi-indirect table block.
        if (biIdx == 0)
        {
            uint32_t newBlock = m_pExt2Fs->findFreeBlock(m_InodeNumber);
            m_pInode->i_block[13] = HOST_TO_LITTLE32(newBlock);
            if (m_pInode->i_block[13] == 0)
            {
                // We had a problem.
                SYSCALL_ERROR(NoSpaceLeftOnDevice);
                return false;
            }

            void *buffer =
                reinterpret_cast<void *>(m_pExt2Fs->readBlock(newBlock));
            ByteSet(buffer, 0, m_pExt2Fs->m_BlockSize);

            // Taken on a new block - update block count (but don't track in
            // m_Blocks, as this is a metadata block).
            m_nMetadataBlocks++;
        }

        // Now we can safely read the bi-indirect block.
        uint32_t bufferBlock = LITTLE_TO_HOST32(m_pInode->i_block[13]);
        uint32_t *pBlock =
            reinterpret_cast<uint32_t *>(m_pExt2Fs->readBlock(bufferBlock));

        // Do we need to start a new indirect block?
        if (indirectIdx == 0)
        {
            uint32_t newBlock = m_pExt2Fs->findFreeBlock(m_InodeNumber);
            pBlock[indirectBlock] = HOST_TO_LITTLE32(newBlock);
            if (pBlock[indirectBlock] == 0)
            {
                // We had a problem.
                SYSCALL_ERROR(NoSpaceLeftOnDevice);
                return false;
            }

            m_pExt2Fs->writeBlock(bufferBlock);

            void *buffer =
                reinterpret_cast<void *>(m_pExt2Fs->readBlock(newBlock));
            ByteSet(buffer, 0, m_pExt2Fs->m_BlockSize);

            // Taken on a new block - update block count (but don't track in
            // m_Blocks, as this is a metadata block).
            m_nMetadataBlocks++;
        }

        // Cache this as it gets clobbered by the readBlock call (using the same
        // buffer).
        uint32_t nIndirectBlockNum = LITTLE_TO_HOST32(pBlock[indirectBlock]);

        // Grab the indirect block.
        pBlock = reinterpret_cast<uint32_t *>(
            m_pExt2Fs->readBlock(nIndirectBlockNum));
        if (pBlock == reinterpret_cast<uint32_t *>(~0))
        {
            ERROR(
                "Could not read block (" << nIndirectBlockNum
                                         << ") that we wanted to add.");
            return false;
        }

        // Set the correct entry.
        pBlock[indirectIdx] = HOST_TO_LITTLE32(blockValue);
        m_pExt2Fs->writeBlock(nIndirectBlockNum);
    }
    else
    {
        // Tri-indirect addressing required.
        FATAL("EXT2: Tri-indirect addressing required, but not implemented.");
        return false;
    }

    trackBlock(blockValue);

    return true;
}

void Ext2Node::fileAttributeChanged(
    size_t size, size_t atime, size_t mtime, size_t ctime)
{
    // Reconstruct the inode from the cached fields.
    uint32_t i_blocks =
        ((m_Blocks.count() + m_nMetadataBlocks) * m_pExt2Fs->m_BlockSize) / 512;
    m_pInode->i_blocks = HOST_TO_LITTLE32(i_blocks);
    m_pInode->i_size = HOST_TO_LITTLE32(size);  /// \todo 4GB files.
    m_pInode->i_atime = HOST_TO_LITTLE32(atime);
    m_pInode->i_mtime = HOST_TO_LITTLE32(mtime);
    m_pInode->i_ctime = HOST_TO_LITTLE32(ctime);

    // Update our internal record of the file size accordingly.
    m_nSize = size;

    // Write updated inode.
    m_pExt2Fs->writeInode(getInodeNumber());
}

void Ext2Node::updateMetadata(uint16_t uid, uint16_t gid, uint32_t perms)
{
    // Avoid wiping out extra mode bits that Pedigree doesn't yet care about.
    uint32_t curr_mode = LITTLE_TO_HOST32(m_pInode->i_mode);
    curr_mode &= ~((1 << 9) - 1);
    curr_mode |= perms;

    m_pInode->i_uid = HOST_TO_LITTLE16(uid);
    m_pInode->i_gid = HOST_TO_LITTLE16(gid);
    m_pInode->i_mode = HOST_TO_LITTLE32(curr_mode);

    // Write updated inode.
    m_pExt2Fs->writeInode(getInodeNumber());
}

void Ext2Node::sync(size_t offset, bool async)
{
    uint32_t nBlock = offset / m_pExt2Fs->m_BlockSize;
    if (nBlock > m_Blocks.count())
        return;
    if (offset > m_nSize)
        return;

    // Sync the block.
    ensureBlockLoaded(nBlock);
    m_pExt2Fs->sync(m_Blocks[nBlock] * m_pExt2Fs->m_BlockSize, async);
}

void Ext2Node::pinBlock(uint64_t location)
{
    uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
    if (nBlock > m_Blocks.count())
        return;
    if (location > m_nSize)
        return;

    ensureBlockLoaded(nBlock);
    m_pExt2Fs->pinBlock(m_Blocks[nBlock]);
}

void Ext2Node::unpinBlock(uint64_t location)
{
    uint32_t nBlock = location / m_pExt2Fs->m_BlockSize;
    if (nBlock > m_Blocks.count())
        return;
    if (location > m_nSize)
        return;

    ensureBlockLoaded(nBlock);
    m_pExt2Fs->unpinBlock(m_Blocks[nBlock]);
}

uint32_t Ext2Node::modeToPermissions(uint32_t mode) const
{
    uint32_t permissions = 0;
    if (mode & EXT2_S_IRUSR)
        permissions |= FILE_UR;
    if (mode & EXT2_S_IWUSR)
        permissions |= FILE_UW;
    if (mode & EXT2_S_IXUSR)
        permissions |= FILE_UX;
    if (mode & EXT2_S_IRGRP)
        permissions |= FILE_GR;
    if (mode & EXT2_S_IWGRP)
        permissions |= FILE_GW;
    if (mode & EXT2_S_IXGRP)
        permissions |= FILE_GX;
    if (mode & EXT2_S_IROTH)
        permissions |= FILE_OR;
    if (mode & EXT2_S_IWOTH)
        permissions |= FILE_OW;
    if (mode & EXT2_S_IXOTH)
        permissions |= FILE_OX;
    return permissions;
}

uint32_t Ext2Node::permissionsToMode(uint32_t permissions) const
{
    uint32_t mode = 0;
    if (permissions & FILE_UR)
        mode |= EXT2_S_IRUSR;
    if (permissions & FILE_UW)
        mode |= EXT2_S_IWUSR;
    if (permissions & FILE_UX)
        mode |= EXT2_S_IXUSR;
    if (permissions & FILE_GR)
        mode |= EXT2_S_IRGRP;
    if (permissions & FILE_GW)
        mode |= EXT2_S_IWGRP;
    if (permissions & FILE_GX)
        mode |= EXT2_S_IXGRP;
    if (permissions & FILE_OR)
        mode |= EXT2_S_IROTH;
    if (permissions & FILE_OW)
        mode |= EXT2_S_IWOTH;
    if (permissions & FILE_OX)
        mode |= EXT2_S_IXOTH;
    return mode;
}
