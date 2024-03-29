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

#ifndef EXT2_H
#define EXT2_H

#include "pedigree/kernel/processor/types.h"

#define EXT2_UNKNOWN 0x0
#define EXT2_FILE 0x1
#define EXT2_DIRECTORY 0x2
#define EXT2_CHAR_DEV 0x3
#define EXT2_BLOCK_DEV 0x4
#define EXT2_FIFO 0x5
#define EXT2_SOCKET 0x6
#define EXT2_SYMLINK 0x7
#define EXT2_MAX 0x8

#define EXT2_STATE_CLEAN 1
#define EXT2_STATE_UNCLEAN 2

#define EXT2_S_IFSOCK 0xC000
#define EXT2_S_IFLNK 0xA000
#define EXT2_S_IFREG 0x8000
#define EXT2_S_IFBLK 0x6000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFCHR 0x2000
#define EXT2_S_IFIFO 0x1000

#define EXT2_S_IRUSR 0x0100
#define EXT2_S_IWUSR 0x0080
#define EXT2_S_IXUSR 0x0040
#define EXT2_S_IRGRP 0x0020
#define EXT2_S_IWGRP 0x0010
#define EXT2_S_IXGRP 0x0008
#define EXT2_S_IROTH 0x0004
#define EXT2_S_IWOTH 0x0002
#define EXT2_S_IXOTH 0x0001

#define EXT2_BAD_INO 0x01          // Bad blocks inode
#define EXT2_ROOT_INO 0x02         // root directory inode
#define EXT2_ACL_IDX_INO 0x03      // ACL index inode (deprecated?)
#define EXT2_ACL_DATA_INO 0x04     // ACL data inode (deprecated?)
#define EXT2_BOOT_LOADER_INO 0x05  // boot loader inode
#define EXT2_UNDEL_DIR_INO 0x06

#define EXT2_LZV1_ALG 0x01
#define EXT2_LZRW3A_ALG 0x02
#define EXT2_GZIP_ALG 0x04
#define EXT2_BZIP2_ALG 0x08
#define EXT2_LZO_ALG 0x10

#define EXT2_SECRM_FL 0x00000001
#define EXT2_UNRM_FL 0x00000002
#define EXT2_COMPR_FL 0x00000004
#define EXT2_SYNC_FL 0x00000008
#define EXT2_IMMUTABLE_FL 0x00000010
#define EXT2_APPEND_FL 0x00000020
#define EXT2_NODUMP_FL 0x00000040
#define EXT2_NOATIME_FL 0x00000080
#define EXT2_DIRTY_FL 0x00000100
#define EXT2_COMPRBLK_FL 0x00000200
#define EXT2_NOCOMPR_FL 0x00000400
#define EXT2_ECOMPR_FL 0x00000800
#define EXT2_BTREE_FL 0x00001000
#define EXT2_INDEX_FL 0x00001000
#define EXT2_IMAGIC_FL 0x00002000
#define EXT3_JOURNAL_DATA_FL 0x00004000
#define EXT2_RESERVED_FL 0x80000000

/** The Ext2 superblock structure. */
struct Superblock
{
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    //   -- EXT2_DYNAMIC_REV Specific --
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    char s_uuid[16];

    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    //   -- Performance Hints         --
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t alignment;
    //   -- Journaling Support        --
    char s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
} __attribute__((packed));

/** The ext2 block group descriptor structure */
struct GroupDesc
{
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t bg_reserved[12];
} __attribute__((packed));

/** An ext2 Inode. */
struct Inode
{
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;  // Top 32-bits of file size.
    uint32_t i_faddr;
    uint8_t i_osd2[12];
} __attribute__((packed));

/** An ext2 directory entry. */
struct Dir
{
    uint32_t d_inode;
    uint16_t d_reclen;
    uint8_t d_namelen;
    uint8_t d_file_type;
    char d_name[256];
} __attribute__((packed));

#endif
