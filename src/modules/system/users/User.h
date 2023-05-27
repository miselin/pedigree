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

#ifndef USER_H
#define USER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

class Group;

/** Defines the properties of a User on the system. */
class EXPORTED_PUBLIC User
{
  public:
    /** Constructor
        \param uid The user's system-wide unique ID.
        \param name The user's username.
        \param fullName The user's full name.
        \param pGroup The default group.
        \param home The user's home directory.
        \param shell The user's default shell.
        \param password Password hash.
        \note Password hash not currently implemented - plaintext only. */
    User(
        size_t uid, const String &username, const String &fullName, Group *pGroup,
        const String &home, const String &shell, const String &password);

    /** The destructor does nothing. */
    virtual ~User();

    /** Adds a group membership. */
    void join(Group *pGroup);

    /** Removes a group membership. */
    void leave(Group *pGroup);

    /** Queries a group membership. */
    bool isMember(Group *pGroup);

    /** (Attempts to) log in as this user. On success this process' user is set
       to this, and the group is set to this user's default group. \return True
       on success, false on failure. */
    bool login(const String &password);

    /** Retrieves the user's UID. */
    size_t getId() const
    {
        return m_Uid;
    }
    /** Retrieves the user's username. */
    const String &getUsername() const
    {
        return m_Username;
    }
    /** Retrieves the user's full name. */
    const String &getFullName() const
    {
        return m_FullName;
    }
    /** Retrieves the user's default group. */
    Group *getDefaultGroup()
    {
        return m_pDefaultGroup;
    }
    /** Retrieves the user's home directory. */
    const String &getHome() const
    {
        return m_Home;
    }
    /** Retrieves the user's default shell. */
    const String &getShell() const
    {
        return m_Shell;
    }

  private:
    /** It doesn't make sense for a User to have a default or copy constructor.
     */
    User();
    User(const User &);
    User &operator=(const User &);

    /** User ID */
    size_t m_Uid;
    /** Username */
    String m_Username;
    /** Full name */
    String m_FullName;
    /** Default group. */
    Group *m_pDefaultGroup;
    /** Home directory. */
    String m_Home;
    /** Default shell. */
    String m_Shell;
    /** Password hash. */
    String m_Password;

    /** Set of groups (excluding default group). */
    List<Group *> m_Groups;
};

#endif
