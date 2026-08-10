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

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"
#include <gtest/gtest.h>

TEST(VFS, AbsolutePathsUseRootFilesystem) {
  VFS vfs;
  RamFs root;
  ASSERT_TRUE(root.initialise(nullptr));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  EXPECT_TRUE(vfs.createDirectory(String("/usr"), 0755));
  EXPECT_TRUE(vfs.createDirectory(String("/usr/bin"), 0755));
  EXPECT_NE(vfs.find(String("/usr/bin")), nullptr);

  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, RelativePathsStillRequireAStartingNode) {
  VFS vfs;
  RamFs root;
  ASSERT_TRUE(root.initialise(nullptr));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  EXPECT_FALSE(vfs.createDirectory(String("usr"), 0755));
  EXPECT_EQ(vfs.find(String("usr")), nullptr);

  vfs.unregisterFilesystem(&root, false);
}

TEST(VFS, NonRootFilesystemsMountUnderMedia) {
  VFS vfs;
  RamFs root;
  RamFs data;
  ASSERT_TRUE(root.initialise(nullptr));
  ASSERT_TRUE(data.initialise(nullptr));

  vfs.registerFilesystem(&data, String("Data Disk"));
  vfs.registerFilesystem(&root, String("root"));
  ASSERT_TRUE(vfs.setRootFilesystem(&root));

  String mountPath;
  ASSERT_TRUE(vfs.getMountPath(&data, mountPath));
  EXPECT_EQ(mountPath, String("/media/Data-Disk"));
  EXPECT_TRUE(vfs.createFile(String("/media/Data-Disk/example"), 0644));
  EXPECT_TRUE(vfs.createFile(String("/root-only"), 0644));

  File* example = vfs.find(String("/media/Data-Disk/example"));
  ASSERT_NE(example, nullptr);
  EXPECT_EQ(example->getFilesystem(), &data);
  EXPECT_EQ(vfs.find(String("/root-only"), data.getRoot()), vfs.find(String("/root-only")));

  vfs.unregisterFilesystem(&data, false);
  vfs.unregisterFilesystem(&root, false);
}
