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

#include <gtest/gtest.h>

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Cord.h"

class StringLogger : public Log::LogCallback
{
  public:
    void callback(const LogCord &cord)
    {
        String str = cord.toString();
        m_Messages += std::string(str.cstr(), str.length());
    }

    const std::string &messages() const
    {
        return m_Messages;
    }

    void reset()
    {
        m_Messages.clear();
    }

  private:
    std::string m_Messages;
};

class PedigreeLog : public ::testing::Test
{
   protected:
    PedigreeLog() = default;
    virtual ~PedigreeLog() = default;

    virtual void SetUp() {
        Log::instance().disableTimestamps();
        Log::instance().installCallback(&m_Logger, true);
    }

    virtual void TearDown() {
        NOTICE("<<wiping out log hash state>>");

        Log::instance().removeCallback(&m_Logger);
        Log::instance().enableTimestamps();

        m_Logger.reset();
    }

    const StringLogger &logger() const
    {
        return m_Logger;
    }

   private:
    StringLogger m_Logger;
};

TEST_F(PedigreeLog, SimpleMessage) {
    NOTICE("Hello world!");

    EXPECT_STREQ(logger().messages().c_str(), "(NN) Hello world!\r\n");
}

TEST_F(PedigreeLog, DuplicatedMessage) {
    for (int i = 0; i < 20; ++i)
    {
        NOTICE("Hello world!");
    }

    NOTICE("A different one");

    EXPECT_STREQ(logger().messages().c_str(), "(NN) Hello world!\r\n(last message+severity repeated 19 times)\r\n(NN) A different one\r\n");
}

TEST_F(PedigreeLog, ManyDuplicatedMessages) {
    for (int i = 0; i < 40; ++i)
    {
        NOTICE("Hello world!");
    }

    NOTICE("A different one");

    EXPECT_STREQ(logger().messages().c_str(), "(NN) Hello world!\r\n(last message+severity repeated 20 times)\r\n(NN) Hello world!\r\n(last message+severity repeated 19 times)\r\n(NN) A different one\r\n");
}
