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

#ifndef GROUP_H
#define GROUP_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"

class User;

/** Defines the properties of a Group on the system.  */
class EXPORTED_PUBLIC Group
{
  public:
    /** Constructor.
        \param gid System-wide unique group ID.
        \param name Group name. */
    Group(size_t gid, String name);
    virtual ~Group();

    /** Adds a user. */
    void join(User *pUser);

    /** Removes a user. */
    void leave(User *pUser);

    /** Queries user membership. */
    bool isMember(User *pUser);

    /** Returns the GID. */
    size_t getId() const
    {
        return m_Gid;
    }
    /** Returns the group name. */
    const String &getName() const
    {
        return m_Name;
    }

  private:
    /** It doesn't make sense for a Group to have public default or copy
     * constructors. */
    Group();
    Group(const Group &);
    Group &operator=(const Group &);

    /** Group ID. */
    size_t m_Gid;
    /** Name. */
    String m_Name;
    /** Group contents. */
    List<User *> m_Users;
};

#endif
