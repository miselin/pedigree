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

#include "User.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

User::User(
    size_t uid, const String &username, const String &fullName, Group *pGroup,
    const String &home, const String &shell, const String &password)
    : m_Uid(uid), m_Username(username), m_FullName(fullName),
      m_pDefaultGroup(pGroup), m_Home(home), m_Shell(shell),
      m_Password(password), m_Groups()
{
}

User::~User()
{
}

void User::join(Group *pGroup)
{
    m_Groups.pushBack(pGroup);
}

void User::leave(Group *pGroup)
{
    for (List<Group *>::Iterator it = m_Groups.begin(); it != m_Groups.end();
         it++)
    {
        if (*it == pGroup)
        {
            m_Groups.erase(it);
            return;
        }
    }
}

bool User::isMember(Group *pGroup)
{
    if (pGroup == m_pDefaultGroup)
        return true;
    for (List<Group *>::Iterator it = m_Groups.begin(); it != m_Groups.end();
         it++)
    {
        if (*it == pGroup)
            return true;
    }
    return false;
}

bool User::login(const String &password)
{
    Process *pProcess =
        Processor::information().getCurrentThread()->getParent();

    if (password == m_Password)
    {
        pProcess->setUser(this);
        pProcess->setGroup(m_pDefaultGroup);

        pProcess->setEffectiveUser(this);
        pProcess->setEffectiveGroup(m_pDefaultGroup);
        return true;
    }
    else
        return false;
}
