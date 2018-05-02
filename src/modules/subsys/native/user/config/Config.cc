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

#include "pedigree/native/config/Config.h"
#include <string.h>

/// \todo make all these available in a header somewhere that isn't the POSIX
/// subsystem
extern "C" {
void pedigree_config_getcolname(
    size_t resultIdx, size_t n, char *buf, size_t bufsz);

void pedigree_config_getstr_n(
    size_t resultIdx, size_t row, size_t n, char *buf, size_t bufsz);

void pedigree_config_getstr_s(
    size_t resultIdx, size_t row, const char *col, char *buf, size_t bufsz);

int pedigree_config_getnum_n(size_t resultIdx, size_t row, size_t n);

int pedigree_config_getnum_s(size_t resultIdx, size_t row, const char *col);

int pedigree_config_getbool_n(size_t resultIdx, size_t row, size_t n);

int pedigree_config_getbool_s(size_t resultIdx, size_t row, const char *col);

int pedigree_config_query(const char *query);

void pedigree_config_freeresult(size_t resultIdx);

int pedigree_config_numcols(size_t resultIdx);

int pedigree_config_numrows(size_t resultIdx);

int pedigree_config_was_successful(size_t resultIdx);

void pedigree_config_get_error_message(size_t resultIdx, char *buf, int buflen);

char *pedigree_config_escape_string(const char *str);
}  // extern "C"

Config::Result::~Result()
{
    pedigree_config_freeresult(m_nResultIdx);
}

bool Config::Result::succeeded()
{
    return !pedigree_config_was_successful(m_nResultIdx);
}

std::string Config::Result::errorMessage(size_t buffSz)
{
    // Allocate a buffer of the given  (or default) size, zero it,
    // get the string in it, then put the buffer contents into a nice string
    // and return the nice string.
    char *pBuffer = new char[buffSz];
    memset(pBuffer, 0, buffSz);
    pedigree_config_get_error_message(m_nResultIdx, pBuffer, buffSz);
    std::string str(pBuffer);
    delete[] pBuffer;
    return str;
}

size_t Config::Result::rows()
{
    return pedigree_config_numrows(m_nResultIdx);
}

size_t Config::Result::cols()
{
    return pedigree_config_numcols(m_nResultIdx);
}

std::string Config::Result::getColumnName(size_t col, size_t buffSz)
{
    // Allocate a buffer of the given  (or default) size, zero it,
    // get the string in it, then put the buffer contents into a nice string
    // and return the nice string.
    char *pBuffer = new char[buffSz];
    memset(pBuffer, 0, buffSz);
    pedigree_config_getcolname(m_nResultIdx, col, pBuffer, buffSz);
    std::string str(pBuffer);
    delete[] pBuffer;
    return str;
}

std::string Config::Result::getStr(size_t row, size_t col, size_t buffSz)
{
    // Allocate a buffer of the given  (or default) size, zero it,
    // get the string in it, then put the buffer contents into a nice string
    // and return the nice string.
    char *pBuffer = new char[buffSz];
    memset(pBuffer, 0, buffSz);
    pedigree_config_getstr_n(m_nResultIdx, row, col, pBuffer, buffSz);
    std::string str(pBuffer);
    delete[] pBuffer;
    return str;
}

size_t Config::Result::getNum(size_t row, size_t col)
{
    return pedigree_config_getnum_n(m_nResultIdx, row, col);
}

bool Config::Result::getBool(size_t row, size_t col)
{
    return pedigree_config_getbool_n(m_nResultIdx, row, col);
}

std::string Config::Result::getStr(size_t row, const char *col, size_t buffSz)
{
    // Allocate a buffer of the given  (or default) size, zero it,
    // get the string in it, then put the buffer contents into a nice string
    // and return the nice string.
    char *pBuffer = new char[buffSz];
    memset(pBuffer, 0, buffSz);
    pedigree_config_getstr_s(m_nResultIdx, row, col, pBuffer, buffSz);
    std::string str(pBuffer);
    delete[] pBuffer;
    return str;
}

size_t Config::Result::getNum(size_t row, const char *col)
{
    return pedigree_config_getnum_s(m_nResultIdx, row, col);
}

bool Config::Result::getBool(size_t row, const char *col)
{
    return pedigree_config_getbool_s(m_nResultIdx, row, col);
}

Config::Result *Config::query(const char *sql)
{
    // Check for null or empty queries
    if (!sql || !*sql)
        return 0;

    // Query the database
    int resultIdx = pedigree_config_query(sql);

    // Check for query errors
    if (resultIdx < 0)
        return 0;

    // Return a new Result
    return new Result(resultIdx);
}
