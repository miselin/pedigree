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

#include "UserManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Group.h"
#include "User.h"
#include "modules/Module.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/VFS.h"

UserManager UserManager::m_Instance;

UserManager::UserManager() : m_Users(), m_Groups() {}

UserManager::~UserManager() {}

static bool readAccountFile(const char* path, String& contents) {
  File* pFile = VFS::instance().find(String(path));
  if (!pFile || pFile->isDirectory()) {
    ERROR("USERS: Unable to read " << path);
    return false;
  }

  size_t offset = 0;
  char buffer[256];
  while (offset < pFile->getSize()) {
    size_t amount = pFile->getSize() - offset;
    if (amount >= sizeof(buffer))
      amount = sizeof(buffer) - 1;

    uint64_t nRead = pFile->read(offset, amount, reinterpret_cast<uintptr_t>(buffer));
    if (!nRead) {
      ERROR("USERS: Unable to read " << path);
      return false;
    }

    contents += String(buffer, nRead, true);
    offset += nRead;
  }

  return true;
}

bool UserManager::initialiseGroups() {
  String contents;
  if (!readAccountFile("/etc/group", contents))
    return false;

  Vector<String> lines;
  contents.tokenise('\n', lines);
  for (auto& line : lines) {
    line.strip();
    if (!line.length() || line[0] == '#')
      continue;

    Vector<String> fields;
    line.tokenise(':', fields);
    if (fields.count() < 3) {
      WARNING("USERS: Ignoring malformed /etc/group entry");
      continue;
    }

    addGroup(StringToUnsignedLong(fields[2].cstr(), 0, 10), fields[0]);
  }

  return true;
}

bool UserManager::initialiseUsers() {
  String contents;
  if (!readAccountFile("/etc/passwd", contents))
    return false;

  Vector<String> lines;
  contents.tokenise('\n', lines);
  for (auto& line : lines) {
    line.strip();
    if (!line.length() || line[0] == '#')
      continue;

    Vector<String> fields;
    line.tokenise(':', fields);
    if (fields.count() < 7) {
      WARNING("USERS: Ignoring malformed /etc/passwd entry");
      continue;
    }

    addUser(StringToUnsignedLong(fields[2].cstr(), 0, 10), fields[0], fields[4],
            StringToUnsignedLong(fields[3].cstr(), 0, 10), fields[5], fields[6]);
  }

  return true;
}

bool UserManager::initialiseMemberships() {
  String contents;
  if (!readAccountFile("/etc/group", contents))
    return false;

  Vector<String> lines;
  contents.tokenise('\n', lines);
  for (auto& line : lines) {
    line.strip();
    if (!line.length() || line[0] == '#')
      continue;

    Vector<String> fields;
    line.tokenise(':', fields);
    if (fields.count() < 4)
      continue;

    Group* pGroup = getGroup(StringToUnsignedLong(fields[2].cstr(), 0, 10));
    if (!pGroup)
      continue;

    Vector<String> members;
    fields[3].tokenise(',', members);
    for (auto& member : members) {
      User* pUser = getUser(member);
      if (pUser)
        pUser->join(pGroup);
    }
  }

  return true;
}

User* UserManager::getUser(size_t id) {
  return m_Users.lookup(id);
}

User* UserManager::getUser(String name) {
  for (Tree<size_t, User*>::Iterator it = m_Users.begin(); it != m_Users.end(); it++) {
    User* pU = it.value();
    if (pU->getUsername() == name)
      return pU;
  }
  return 0;
}

Group* UserManager::getGroup(size_t id) {
  return m_Groups.lookup(id);
}

Group* UserManager::getGroup(String name) {
  for (Tree<size_t, Group*>::Iterator it = m_Groups.begin(); it != m_Groups.end(); it++) {
    Group* pG = it.value();
    if (pG->getName() == name)
      return pG;
  }
  return 0;
}

void UserManager::addUser(size_t uid, String username, String fullName, size_t group, String home,
                          String shell) {
  // Check for duplicates.
  if (getUser(uid)) {
    WARNING("USERS: Not inserting user '" << username << "' with uid " << uid << ": duplicate.");
    return;
  }

  // Check that the given group exists.
  Group* pGroup = getGroup(group);
  if (!pGroup) {
    WARNING("USERS: Not inserting user '" << username << "': group " << group
                                          << " does not exist.");
    return;
  }

  NOTICE("USERS: Adding user {" << uid << ",'" << username << "','" << fullName << "'}");
  User* pUser = new User(uid, username, fullName, pGroup, home, shell);
  pGroup->join(pUser);
  m_Users.insert(uid, pUser);
}

void UserManager::addGroup(size_t gid, String name) {
  // Check for duplicates.
  if (getGroup(gid)) {
    WARNING("USERS: Not inserting group '" << name << "' with gid " << gid << ": duplicate.");
    return;
  }

  NOTICE("USERS: Adding group {" << gid << ",'" << name << "'}");
  Group* pGroup = new Group(gid, name);
  m_Groups.insert(gid, pGroup);
}

void UserManager::initialise() {
  if (!initialiseGroups() || !initialiseUsers() || !initialiseMemberships()) {
    FATAL("USERS: Unable to load /etc/passwd and /etc/group");
    return;
  }

  User* pUser = getUser(0);
  if (!pUser) {
    FATAL("USERS: Unable to set default user (no such user for uid 0)");
    return;
  }
  Process* pProcess = Processor::information().getCurrentThread()->getParent();

  pProcess->setUser(pUser);
  pProcess->setGroup(pUser->getDefaultGroup());
}

static bool init() {
  // Initialise user/group configuration.
  UserManager::instance().initialise();

  return true;
}

static void destroy() {}

MODULE_INFO("users", &init, &destroy, "mountroot");
