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

#include "pedigree/kernel/debugger/SerialIO.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/utilities/StaticString.h"

SerialIO::SerialIO(Serial *pSerial)
    : m_UpperCliLimit(0), m_LowerCliLimit(0), m_nWidth(80), m_nHeight(25),
      m_nCursorX(0), m_nCursorY(0), m_nOldCursorX(0), m_nOldCursorY(0),
      m_ForeColour(DebuggerIO::Red), m_BackColour(DebuggerIO::Red),
      m_pSerial(pSerial), m_bCli(false)
{
    //  initialise();
}

SerialIO::~SerialIO()
{
    destroy();
}

void SerialIO::initialise()
{
    // Save cursor and attributes.
    //   m_pSerial->write_str("\033[s");

#if !SERIAL_IS_FILE
    // Read cursor location.
    readCursor();
    m_nOldCursorX = m_nCursorX;
    m_nOldCursorY = m_nCursorY;
    NOTICE(Hex << "oldx: " << m_nOldCursorX << ", y: " << m_nOldCursorY);

#endif

    // Push screen contents.
    m_pSerial->write_str("\033[?1049h");

    // Move the cursor back to the top left.
    //   m_pSerial->write_str("\033[0;0H");

    // Enable line wrapping.
    m_pSerial->write_str("\033[7h");
}

void SerialIO::destroy()
{
    // Pop screen contents.
    m_pSerial->write_str("\033[?1049l");

    // Disable scrolling.
    m_pSerial->write_str("\033[0;0r");

    // Load cursor and attributes.
    //   m_pSerial->write_str("\033[u");

    // Just reset the terminal.
    //   m_pSerial->write_str("\033c");

    // Set the cursor
    TinyStaticString str;
    str += "\033[";
    str += m_nOldCursorY;
    str += ";";
    str += m_nOldCursorX;
    str += "H";
    m_pSerial->write_str(str);
}

void SerialIO::setCliUpperLimit(size_t nlines)
{
    // Do a quick sanity check.
    if (nlines < m_nHeight)
        m_UpperCliLimit = nlines;
}

void SerialIO::setCliLowerLimit(size_t nlines)
{
    // Do a quick sanity check.
    if (nlines < m_nHeight)
        m_LowerCliLimit = nlines;
}

void SerialIO::enableCli()
{
    // Clear the screen.
    m_pSerial->write_str("\033[2J");

    // Set the scrollable region.
    TinyStaticString str("\033[");
    str += (m_UpperCliLimit + 1);
    str += ';';
    str += m_nHeight - m_LowerCliLimit;
    str += 'r';
    m_pSerial->write_str(str);

    //   Set the cursor to just below the upper limit.
    str = "\033[";
    str += (m_UpperCliLimit + 1);
    str += ";0H";
    m_pSerial->write_str(str);
    m_pCommand[0] = '\0';

    m_bCli = true;
}

void SerialIO::disableCli()
{
    // Clear the screen.
    m_pSerial->write_str("\033[2J");

    // Disable scrolling.
    m_pSerial->write_str("\033[0;0r");

    // Reposition the cursor.
    //   m_pSerial->write_str("\033[0;0H");

    m_bCli = false;
}

char SerialIO::getChar()
{
    char c = m_pSerial->read();
    if (c == 0x7f)  // We hardcode DEL -> BACKSPACE.
        return 0x08;
    if (c == '\033')
    {
        // Agh! VT100 code!
        ERROR("VT100 code!!");
        //     while ( c != 'R' )
        //       c = m_pSerial->read();
        return 0;
    }
    return c;
}

void SerialIO::drawHorizontalLine(
    char c, size_t row, size_t colStart, size_t colEnd,
    DebuggerIO::Colour foreColour, DebuggerIO::Colour backColour)
{
    saveCursor();

    // colEnd must be bigger than colStart.
    if (colStart > colEnd)
    {
        size_t tmp = colStart;
        colStart = colEnd;
        colEnd = tmp;
    }

    if (colEnd >= m_nWidth)
        colEnd = m_nWidth - 1;
    if (static_cast<int32_t>(colStart) < 0)
        colStart = 0;
    if (row >= m_nHeight)
        row = m_nHeight - 1;
    if (static_cast<int32_t>(row) < 0)
        row = 0;

    startColour(foreColour, backColour);

    // Here we can do clever stuff. If we're erasing to the start or the end of
    // the line, and we're trying to clear it (c == ' ') vt100 has some nice
    // codes that make our life easier and faster.
    if (colEnd == m_nWidth - 1 && c == ' ')
    {
        // Position the cursor at the specified column, row.
        TinyStaticString cmd("\033[");
        cmd += (row + 1);
        cmd += ';';
        cmd += (colStart + 1);
        cmd += 'H';
        m_pSerial->write_str(cmd);

        // Erase to the end of line.
        m_pSerial->write_str("\033[K");
    }
    else if (colStart == 0 && c == ' ')
    {
        // Position the cursor where the line is supposed to *end*.
        TinyStaticString cmd("\033[");
        cmd += (row + 1);
        cmd += ';';
        cmd += (colEnd + 1);
        cmd += 'H';
        m_pSerial->write_str(cmd);

        // Erase backwards to the start of line.
        m_pSerial->write_str("\033[1K");
    }
    else
    {
        // Position the cursor at the specified column, row.
        TinyStaticString cmd("\033[");
        cmd += (row + 1);
        cmd += ';';
        cmd += (colStart + 1);
        cmd += 'H';
        m_pSerial->write_str(cmd);

        // Write each character seperately.
        for (size_t i = colStart; i <= colEnd; i++)
        {
            m_pSerial->write(c);
        }
    }

    endColour();

    unsaveCursor();
}

void SerialIO::drawVerticalLine(
    char c, size_t col, size_t rowStart, size_t rowEnd,
    DebuggerIO::Colour foreColour, DebuggerIO::Colour backColour)
{
    // rowEnd must be bigger than rowStart.
    if (rowStart > rowEnd)
    {
        size_t tmp = rowStart;
        rowStart = rowEnd;
        rowEnd = tmp;
    }

    if (rowEnd >= m_nHeight)
        rowEnd = m_nHeight - 1;
    if (static_cast<int32_t>(rowStart) < 0)
        rowStart = 0;
    if (col >= m_nWidth)
        col = m_nWidth - 1;
    if (static_cast<int32_t>(col) < 0)
        col = 0;

    // TODO position cursor, draw line.
}

void SerialIO::drawString(
    const char *str, size_t row, size_t col, DebuggerIO::Colour foreColour,
    DebuggerIO::Colour backColour)
{
    saveCursor();

    // Position the cursor at the specified column, row.
    TinyStaticString cmd("\033[");
    cmd += (row + 1);
    cmd += ';';
    cmd += (col + 1);
    cmd += 'H';
    m_pSerial->write_str(cmd);

    startColour(foreColour, backColour);

    m_pSerial->write_str(str);
    endColour();
    unsaveCursor();
}

void SerialIO::enableRefreshes()
{
    m_bRefreshesEnabled = true;
}

void SerialIO::disableRefreshes()
{
    m_bRefreshesEnabled = false;
}

void SerialIO::scroll()
{
}

void SerialIO::moveCursor()
{
}

void SerialIO::cls()
{
    // Clear the screen.
    m_pSerial->write_str("\033[2J");
}

void SerialIO::putChar(
    char c, DebuggerIO::Colour foreColour, DebuggerIO::Colour backColour)
{
    startColour(foreColour, backColour);

    if (c == 0x08)  // Backspace - handle differently.
    {
        readCursor();
        // Go back one space.
        // Are we at the screen edge?
        if (m_nCursorX == 1)
        {
            m_nCursorX = m_nWidth - 1;
            m_nCursorY--;
        }
        else
        {
            m_nCursorX--;
        }
        setCursor();
        m_pSerial->write(' ');
        setCursor();
    }
    else
    {
        /// \todo This code below is costly and slow, but without it we can't
        /// line-wrap. Sort this out.
        //     if (m_bCli) // If we're not in CLI mode we dont bother with the
        //     costly wrap-check.
        //     {
        //       readCursor();
        //       if (m_nCursorX == m_nWidth)
        //         m_pSerial->write_str("\n\r");
        //     }
        if (c == '\n')  // Newline - output a '\r' as well.
            m_pSerial->write('\r');
        m_pSerial->write(c);
    }
    endColour();
}

void SerialIO::forceRefresh()
{
}

void SerialIO::startColour(
    DebuggerIO::Colour foreColour, DebuggerIO::Colour backColour)
{
    if (foreColour == m_ForeColour && backColour == m_BackColour)
        return;

    m_ForeColour = foreColour;
    m_BackColour = backColour;

    m_pSerial->write_str("\033[");
    switch (foreColour)
    {
        case DebuggerIO::Black:
            m_pSerial->write_str("30");
            break;
        case DebuggerIO::Red:
            m_pSerial->write_str("31");
            break;
        case DebuggerIO::Green:
            m_pSerial->write_str("32");
            break;
        case DebuggerIO::Yellow:
            m_pSerial->write_str("1;33");
            break;  // It's actually brown.
        case DebuggerIO::Blue:
            m_pSerial->write_str("34");
            break;
        case DebuggerIO::Magenta:
            m_pSerial->write_str("35");
            break;
        case DebuggerIO::Cyan:
            m_pSerial->write_str("36");
            break;
        case DebuggerIO::White:
            m_pSerial->write_str("37");
            break;
        case DebuggerIO::DarkGrey:
            m_pSerial->write_str("1;30");
            break;
        case DebuggerIO::LightRed:
            m_pSerial->write_str("1;31");
            break;
        case DebuggerIO::LightGreen:
            m_pSerial->write_str("1;32");
            break;
        case DebuggerIO::LightBlue:
            m_pSerial->write_str("1;34");
            break;
        case DebuggerIO::LightMagenta:
            m_pSerial->write_str("1;35");
            break;
        case DebuggerIO::LightCyan:
            m_pSerial->write_str("1;36");
            break;
        default:
            m_pSerial->write('1');
    }
    m_pSerial->write_str(";");
    switch (backColour)
    {
        case DebuggerIO::Black:
            m_pSerial->write_str("40");
            break;
        case DebuggerIO::Red:
            m_pSerial->write_str("41");
            break;
        case DebuggerIO::Green:
            m_pSerial->write_str("42");
            break;
        case DebuggerIO::DarkGrey:
            m_pSerial->write_str("43");
            break;  // It's actually brown.
        case DebuggerIO::Blue:
            m_pSerial->write_str("44");
            break;
        case DebuggerIO::Magenta:
            m_pSerial->write_str("45");
            break;
        case DebuggerIO::Cyan:
            m_pSerial->write_str("46");
            break;
        case DebuggerIO::White:
            m_pSerial->write_str("47");
            break;
        default:
            m_pSerial->write('1');
    }
    m_pSerial->write('m');
}

char SerialIO::getCharNonBlock()
{
    return m_pSerial->readNonBlock();
}

void SerialIO::endColour()
{
    m_pSerial->write_str("\033[0m");
}

void SerialIO::readCursor()
{
    // Ask the device wherethe cursor is.
    m_pSerial->write_str("\033[6n");

    // Expect a string of the form "\033[%d;%dR"
    char c = m_pSerial->read();
    if (c != '\033')
    {
        ERROR("SerialIO - device responded incorrectly to size query.");
        return;
    }
    if (m_pSerial->read() != '[')
    {
        ERROR("SerialIO - device responded incorrectly to size query.");
        return;
    }

    TinyStaticString str;
    c = m_pSerial->read();
    while (c >= '0' && c <= '9')
    {
        str += c;
        c = m_pSerial->read();
    }

    m_nCursorY = str.intValue();

    if (c != ';')
    {
        ERROR("SerialIO - device responded incorrectly to size query.");
        return;
    }

    str = "";
    c = m_pSerial->read();
    while (c >= '0' && c <= '9')
    {
        str += c;
        c = m_pSerial->read();
    }

    m_nCursorX = str.intValue();
    if (c != 'R')
    {
        ERROR("SerialIO - device responded incorrectly to size query.");
        return;
    }
}

void SerialIO::setCursor()
{
    TinyStaticString str("\033[");
    str += m_nCursorY;
    str += ";";
    str += m_nCursorX;
    str += "H";
    m_pSerial->write_str(str);
}

void SerialIO::saveCursor()
{
    //   readCursor();
    m_pSerial->write_str("\033[s");
}

void SerialIO::unsaveCursor()
{
    //   setCursor();
    m_pSerial->write_str("\033[u");
}

void SerialIO::readDimensions()
{
    // Read old cursor position
    readCursor();
#if SERIAL_IS_FILE
    m_nOldCursorX = m_nCursorX;
    m_nOldCursorY = m_nCursorY;
#endif
    // Move the cursor off the bottom right somewhere. The device will clamp to
    // its available area.
    m_nCursorY = 10000;
    m_nCursorX = 10000;
    setCursor();
    readCursor();
    m_nWidth = m_nCursorX;
    m_nHeight = m_nCursorY;

    // Move the cursor back to the top left.
    m_pSerial->write_str("\033[0;0H");
}
