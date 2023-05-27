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

#include "TextIO.h"
#include "modules/system/vfs/File.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Vga.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

class Filesystem;
class Process;

/// \todo these come from somewhere - expose them properly
#define ALT_KEY (1ULL << 60)
#define SHIFT_KEY (1ULL << 61)
#define CTRL_KEY (1ULL << 62)
#define SPECIAL_KEY (1ULL << 63)

static int startFlipThread(void *param);

TextIO::TextIO(String str, size_t inode, Filesystem *pParentFS, File *pParent)
    : File(str, 0, 0, 0, inode, pParentFS, 0, pParent), m_bInitialised(false),
      m_bControlSeq(false), m_bBracket(false), m_bParenthesis(false),
      m_bParams(false), m_bQuestionMark(false), m_CursorX(0), m_CursorY(0),
      m_SavedCursorX(0), m_SavedCursorY(0), m_ScrollStart(0), m_ScrollEnd(0),
      m_LeftMargin(0), m_RightMargin(0), m_CurrentParam(0), m_Params(),
      m_Fore(TextIO::LightGrey), m_Back(TextIO::Black),
      m_Backbuffer("TextIO Backbuffer"), m_pFramebuffer(0), m_pBackbuffer(0),
      m_pVga(0), m_TabStops(), m_OutBuffer(TEXTIO_BUFFER_SIZE), m_G0('B'),
      m_G1('B'), m_bUtf8(false), m_nCharacter(0), m_nUtf8Handled(0),
      m_bActive(false), m_Lock(false), m_bOwnsConsole(false),
      m_InputMode(TextIO::Standard)
{
    size_t backbufferSize =
        BACKBUFFER_STRIDE * BACKBUFFER_ROWS * sizeof(VgaCell);
    size_t backbufferPages =
        (backbufferSize + PhysicalMemoryManager::getPageSize() - 1) /
        PhysicalMemoryManager::getPageSize();

    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_Backbuffer, backbufferPages, 0,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write))
    {
        ERROR("TextIO: failed to allocate backbuffer!");
    }
    else
    {
        m_pBackbuffer =
            reinterpret_cast<VgaCell *>(m_Backbuffer.virtualAddress());
    }

    clearBackbuffer();

    // r/w for root user/group, no access for everyone else.
    setPermissionsOnly(FILE_GR | FILE_GW | FILE_UR | FILE_UW);
    setUidOnly(0);
    setGidOnly(0);

    InputManager::instance().installCallback(
        InputManager::Key, inputCallback, this);
    InputManager::instance().installCallback(
        InputManager::MachineKey, inputCallback, this);
}

TextIO::~TextIO()
{
    // Join to the flip thread now that we're terminating.
    m_bInitialised = false;
    m_pFlipThread->join();

    m_pBackbuffer = 0;
    m_Backbuffer.free();

    InputManager::instance().removeCallback(inputCallback, this);
}

bool TextIO::initialise(bool bClear)
{
    LockGuard<Mutex> guard(m_Lock);

    if (m_bInitialised)
    {
        m_bInitialised = false;
        m_pFlipThread->join();
    }

    // Move into not-initialised mode, reset any held state.
    m_bInitialised = false;
    m_bActive = false;
    m_bControlSeq = false;
    m_bBracket = false;
    m_bParams = false;
    m_bQuestionMark = false;
    m_pFramebuffer = 0;
    m_CurrentParam = 0;
    m_CursorX = m_CursorY = 0;
    m_ScrollStart = m_ScrollEnd = 0;
    m_LeftMargin = m_RightMargin = 0;
    m_SavedCursorX = m_SavedCursorY = 0;
    m_CurrentModes = 0;
    ByteSet(m_Params, 0, sizeof(size_t) * MAX_TEXTIO_PARAMS);
    ByteSet(m_TabStops, 0, BACKBUFFER_STRIDE);
    m_InputMode = Standard;

    m_pVga = Machine::instance().getVga(0);
    if (m_pVga)
    {
        m_pVga->setLargestTextMode();
        m_pFramebuffer = *m_pVga;
        if (m_pFramebuffer != 0)
        {
            if (bClear)
            {
                if (isPrimary())
                {
                    ByteSet(
                        m_pFramebuffer, 0,
                        m_pVga->getNumRows() * m_pVga->getNumCols() *
                            sizeof(uint16_t));
                }

                clearBackbuffer();
            }

            m_bInitialised = true;
            m_ScrollStart = 0;
            m_ScrollEnd = m_pVga->getNumRows() - 1;
            m_LeftMargin = 0;
            m_RightMargin = m_pVga->getNumCols();

            m_CurrentModes = AnsiVt52 | CharacterSetG0;

            // Set default tab stops.
            for (size_t i = 0; i < BACKBUFFER_STRIDE; i += 8)
                m_TabStops[i] = '|';

            m_pVga->clearControl(Vga::Blink);

            m_G0 = m_G1 = 'B';

            m_NextInterval = BLINK_OFF_PERIOD;
        }
    }

    if (m_bInitialised)
    {
        Process *parent =
            Processor::information().getCurrentThread()->getParent();
        m_pFlipThread = new Thread(parent, startFlipThread, this);
        m_pFlipThread->setName("TextIO flip thread");
    }

    return m_bInitialised;
}

void TextIO::writeStr(const char *s, size_t len)
{
    if (!m_bInitialised)
    {
        FATAL("TextIO misused: successfully call initialise() first.");
    }

    if (!s)
    {
        ERROR("TextIO: null string passed in.");
        return;
    }

    m_bActive = true;

    const char *orig = s;
    while ((*s) && (len--))
    {
        // UTF8 -> UTF32 conversion.
        uint8_t byte = *reinterpret_cast<const uint8_t *>(s);
        if (m_bUtf8)
        {
            if (m_nUtf8Handled >= 6)
            {
                m_nUtf8Handled -= 6;
                m_nCharacter |= (byte & 0x3F) << m_nUtf8Handled;

                if (m_nUtf8Handled)
                {
                    ++s;
                    continue;
                }
            }

            if ((m_nUtf8Handled == 0) || ((byte & 0xC0) != 0x80))
            {
                if (m_nUtf8Handled > 0)
                    ERROR("TextIO: expected a continuation byte, but didn't "
                          "get one");

                // All good to use m_nCharacter now!
                m_bUtf8 = false;

                // If we terminated due to a byte that is not a continuation, we
                // need to adjust the string pointer so we end up handling this
                // character again, as a character that is not part of this UTF8
                // sequence.
                if (((byte & 0xC0) != 0x80) && (s != orig))
                {
                    --s;
                    ++len;
                }

                // Ignore the codepoint if it is bad.
                if (m_nCharacter > 0x10FFFF)
                {
                    ERROR("TextIO: invalid UTF8 sequence encountered.");
                    continue;
                }
            }
            else if (m_nUtf8Handled < 6)
            {
                ERROR(
                    "TextIO: too many continuation bytes for a UTF8 sequence!");
                m_bUtf8 = false;
                ++s;
                continue;
            }
        }
        else if ((byte & 0xC0) == 0xC0)
        {
            m_bUtf8 = true;

            uint8_t thisByte = *reinterpret_cast<const uint8_t *>(s);
            if ((thisByte & 0xF8) == 0xF0)
            {
                // 4-byte sequence.
                m_nCharacter = (thisByte & 0x7) << 18;
                m_nUtf8Handled = 18;
            }
            else if ((thisByte & 0xF0) == 0xE0)
            {
                // 3-byte sequence.
                m_nCharacter = (thisByte & 0xF) << 12;
                m_nUtf8Handled = 12;
            }
            else if ((thisByte & 0xE0) == 0xC0)
            {
                // 2-byte sequence.
                m_nCharacter = (thisByte & 0x1F) << 6;
                m_nUtf8Handled = 6;
            }
            else
            {
                ERROR("TextIO: invalid UTF8 leading byte (possible 5- or "
                      "6-byte sequence?)");
                m_bUtf8 = false;
            }

            ++s;
            continue;
        }
        else if ((byte & 0x80) == 0x80)
        {
            ERROR(
                "TextIO: invalid ASCII character "
                << byte << " (not a UTF8 leading byte)");
            ++s;
            continue;
        }
        else
            m_nCharacter = *s;

        if (m_bControlSeq && m_bBracket)
        {
            switch (m_nCharacter)
            {
                case '"':
                case '$':
                case '!':
                case '>':
                    // Eat unhandled characters.
                    break;

                case 0x08:
                    doBackspace();
                    break;

                case '\n':
                case 0x0B:
                    if (m_CurrentModes & LineFeedNewLine)
                        doCarriageReturn();
                    doLinefeed();
                    break;

                case '\r':
                    doCarriageReturn();
                    break;

                case '?':
                    m_bQuestionMark = true;
                    break;

                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    m_Params[m_CurrentParam] =
                        (m_Params[m_CurrentParam] * 10) + (m_nCharacter - '0');
                    m_bParams = true;
                    break;

                case ';':
                    ++m_CurrentParam;
                    if (m_CurrentParam >= MAX_TEXTIO_PARAMS)
                        FATAL("TextIO: too many parameters!");
                    break;

                case 'A':
                    // Cursor up.
                    if (m_CursorY)
                    {
                        if (m_bParams && m_Params[0])
                            m_CursorY -= m_Params[0];
                        else
                            --m_CursorY;
                    }

                    if (m_CursorY < m_ScrollStart)
                        m_CursorY = m_ScrollStart;

                    m_bControlSeq = false;
                    break;

                case 'B':
                    // Cursor down.
                    if (m_bParams && m_Params[0])
                        m_CursorY += m_Params[0];
                    else
                        ++m_CursorY;

                    if (m_CursorY > m_ScrollEnd)
                        m_CursorY = m_ScrollEnd;

                    m_bControlSeq = false;
                    break;

                case 'C':
                    // Cursor right.
                    if (m_bParams && m_Params[0])
                        m_CursorX += m_Params[0];
                    else
                        ++m_CursorX;

                    if (m_CursorX >= m_RightMargin)
                        m_CursorX = m_RightMargin - 1;

                    m_bControlSeq = false;
                    break;

                case 'D':
                    // Cursor left.
                    if (m_CursorX)
                    {
                        if (m_bParams && m_Params[0])
                            m_CursorX -= m_Params[0];
                        else
                            --m_CursorX;
                    }

                    if (m_CursorX < m_LeftMargin)
                        m_CursorX = m_LeftMargin;

                    m_bControlSeq = false;
                    break;

                case 'H':
                case 'f':
                    // CUP/HVP commands
                    if (m_bParams)
                    {
                        size_t xmove = m_Params[1] ? m_Params[1] - 1 : 0;
                        size_t ymove = m_Params[0] ? m_Params[0] - 1 : 0;

                        // Set X/Y
                        goHome(xmove, ymove);
                    }
                    else
                    {
                        // Reset X/Y
                        goHome();
                    }

                    m_bControlSeq = false;
                    break;

                case 'J':
                    if ((!m_bParams) || (!m_Params[0]))
                    {
                        eraseEOS();
                    }
                    else if (m_Params[0] == 1)
                    {
                        eraseSOS();
                    }
                    else if (m_Params[0] == 2)
                    {
                        // Erase entire screen, move to home.
                        eraseScreen(' ');
                        goHome();
                    }
                    m_bControlSeq = false;
                    break;

                case 'K':
                    if ((!m_bParams) || (!m_Params[0]))
                    {
                        eraseEOL();
                    }
                    else if (m_Params[0] == 1)
                    {
                        // Erase to start of line.
                        eraseSOL();
                    }
                    else if (m_Params[0] == 2)
                    {
                        // Erase entire line.
                        eraseLine();
                    }
                    m_bControlSeq = false;
                    break;

                case 'c':
                    if (m_Params[0])
                    {
                        ERROR("TextIO: Device Attributes command with non-zero "
                              "parameter.");
                    }
                    else
                    {
                        // We mosly support the 'Advanced Video Option'.
                        // (apart from underline/blink)
                        const char *attribs = "\033[?1;2c";
                        m_OutBuffer.write(
                            const_cast<char *>(attribs), StringLength(attribs));
                    }
                    m_bControlSeq = false;
                    break;

                case 'g':
                    if (m_Params[0])
                    {
                        if (m_Params[0] == 3)
                        {
                            ByteSet(m_TabStops, 0, BACKBUFFER_STRIDE);
                        }
                    }
                    else
                    {
                        m_TabStops[m_CursorX] = 0;
                    }
                    m_bControlSeq = false;
                    break;

                case 'h':
                case 'l':
                {
                    int modesToChange = 0;

                    if (m_bQuestionMark & m_bParams)
                    {
                        for (size_t i = 0; i <= m_CurrentParam; ++i)
                        {
                            switch (m_Params[i])
                            {
                                case 1:
                                    modesToChange |= CursorKey;
                                    break;
                                case 2:
                                    modesToChange |= AnsiVt52;
                                    break;
                                case 3:
                                    modesToChange |= Column;
                                    break;
                                case 4:
                                    modesToChange |= Scrolling;
                                    break;
                                case 5:
                                    modesToChange |= Screen;
                                    break;
                                case 6:
                                    modesToChange |= Origin;
                                    break;
                                case 7:
                                    modesToChange |= AutoWrap;
                                    break;
                                case 8:
                                    modesToChange |= AutoRepeat;
                                    break;
                                case 9:
                                    modesToChange |= Interlace;
                                    break;
                                default:
                                    WARNING(
                                        "TextIO: unknown 'DEC Private Mode "
                                        "Set' mode '"
                                        << m_Params[i] << "'");
                                    break;
                            }
                        }
                    }
                    else if (m_bParams)
                    {
                        for (size_t i = 0; i <= m_CurrentParam; ++i)
                        {
                            switch (m_Params[i])
                            {
                                case 20:
                                    modesToChange |= LineFeedNewLine;
                                    break;
                                default:
                                    WARNING(
                                        "TextIO: unknown 'Set Mode' mode '"
                                        << m_Params[i] << "'");
                                    break;
                            }
                        }
                    }

                    if (m_nCharacter == 'h')
                    {
                        // Set modes.
                        m_CurrentModes |= modesToChange;

                        // Setting modes
                        if (modesToChange & Origin)
                        {
                            // Reset origin to margins.
                            m_CursorX = m_LeftMargin;
                            m_CursorY = m_ScrollStart;
                        }
                        else if (modesToChange & Column)
                        {
                            m_RightMargin = BACKBUFFER_COLS_WIDE;

                            // Clear screen as a side-effect.
                            eraseScreen(' ');

                            // Reset margins.
                            m_LeftMargin = 0;
                            m_ScrollStart = 0;
                            m_ScrollEnd = BACKBUFFER_ROWS - 1;

                            // Home the cursor.
                            m_CursorX = 0;
                            m_CursorY = 0;
                        }
                    }
                    else
                    {
                        // Reset modes.
                        m_CurrentModes &= ~(modesToChange);

                        // Resetting modes
                        if (modesToChange & Origin)
                        {
                            // Reset origin to top left corner.
                            m_CursorX = 0;
                            m_CursorY = 0;
                        }
                        else if (modesToChange & Column)
                        {
                            m_RightMargin = BACKBUFFER_COLS_NORMAL;

                            // Clear screen as a side-effect.
                            eraseScreen(' ');

                            // Reset margins.
                            m_LeftMargin = 0;
                            m_ScrollStart = 0;
                            m_ScrollEnd = BACKBUFFER_ROWS - 1;

                            // Home the cursor.
                            m_CursorX = 0;
                            m_CursorY = 0;
                        }
                    }

                    m_bControlSeq = false;
                }
                break;

                case 'm':
                    for (size_t i = 0; i <= m_CurrentParam; ++i)
                    {
                        switch (m_Params[i])
                        {
                            case 0:
                                // Reset all attributes.
                                m_Fore = LightGrey;
                                m_Back = Black;
                                m_CurrentModes &= ~(Inverse | Bright | Blink);
                                break;

                            case 1:
                                if (!(m_CurrentModes & Bright))
                                {
                                    m_CurrentModes |= Bright;
                                }
                                break;

                            case 2:
                                if (m_CurrentModes & Bright)
                                {
                                    m_CurrentModes &= ~Bright;
                                }
                                break;

                            case 5:
                                // Set blinking text.
                                if (!(m_CurrentModes & Blink))
                                {
                                    m_CurrentModes |= Blink;
                                }
                                break;

                            case 7:
                                if (!(m_CurrentModes & Inverse))
                                {
                                    m_CurrentModes |= Inverse;
                                }
                                break;

                            case 30:
                            case 31:
                            case 32:
                            case 33:
                            case 34:
                            case 35:
                            case 36:
                            case 37:
                                setColour(
                                    &m_Fore, m_Params[i] - 30,
                                    m_CurrentModes & Bright);
                                break;
                            case 38:
                                if (m_Params[i + 1] == 5)
                                {
                                    setColour(
                                        &m_Fore, m_Params[i + 2],
                                        m_CurrentModes & Bright);
                                    i += 3;
                                }
                                break;
                            case 39:
                                setColour(&m_Back, 7, m_CurrentModes & Bright);
                                break;

                            case 40:
                            case 41:
                            case 42:
                            case 43:
                            case 44:
                            case 45:
                            case 46:
                            case 47:
                                setColour(&m_Back, m_Params[i] - 40);
                                break;
                            case 48:
                                if (m_Params[i + 1] == 5)
                                {
                                    setColour(&m_Back, m_Params[i + 2]);
                                    i += 3;
                                }
                                break;
                            case 49:
                                setColour(&m_Back, 0);
                                break;

                            case 90:
                            case 91:
                            case 92:
                            case 93:
                            case 94:
                            case 95:
                            case 96:
                            case 97:
                                setColour(&m_Fore, m_Params[i] - 90, true);
                                break;

                            case 100:
                            case 101:
                            case 102:
                            case 103:
                            case 104:
                            case 105:
                            case 106:
                            case 107:
                                setColour(&m_Back, m_Params[i] - 100, true);
                                break;

                            default:
                                WARNING(
                                    "TextIO: unhandled 'Set Attribute Mode' "
                                    "command "
                                    << Dec << m_Params[i] << Hex << ".");
                                break;
                        }
                    }
                    m_bControlSeq = false;
                    break;

                case 'n':
                    switch (m_Params[0])
                    {
                        case 5:
                        {
                            // Report ready with no malfunctions detected.
                            const char *status = "\033[0n";
                            m_OutBuffer.write(
                                const_cast<char *>(status),
                                StringLength(status));
                        }
                        break;
                        case 6:
                        {
                            // Report cursor position.
                            // CPR - \e[ Y ; X R
                            NormalStaticString response("\033[");

                            ssize_t reportX = m_CursorX + 1;
                            ssize_t reportY = m_CursorY + 1;

                            if (m_CurrentModes & Origin)
                            {
                                // Only report relative if the cursor is within
                                // the margins and scroll region! Otherwise,
                                // absolute.
                                if ((reportX > m_LeftMargin) &&
                                    (reportX <= m_RightMargin))
                                    reportX -= m_LeftMargin;
                                if ((reportY > m_ScrollStart) &&
                                    (reportY <= m_ScrollEnd))
                                    reportY -= m_ScrollStart;
                            }

                            response.append(reportY);
                            response.append(";");
                            response.append(reportX);
                            response.append("R");
                            m_OutBuffer.write(
                                const_cast<char *>(
                                    static_cast<const char *>(response)),
                                response.length());
                        }
                        break;
                        default:
                            NOTICE(
                                "TextIO: unknown device status request "
                                << Dec << m_Params[0] << Hex << ".");
                            break;
                    }
                    m_bControlSeq = false;
                    break;

                case 'p':
                    // Depending on parameters and symbols in the sequence, this
                    // could be "Set Conformance Level" (DECSCL),
                    // "Soft Terminal Reset" (DECSTR), etc, etc... so ignore for
                    // now.
                    /// \todo Should we handle this?
                    WARNING("TextIO: dropping command after seeing 'p' command "
                            "sequence terminator.");
                    m_bControlSeq = false;
                    break;

                case 'q':
                    // Load LEDs
                    /// \todo hook in to Keyboard::setLedState!
                    m_bControlSeq = false;
                    break;

                case 'r':
                    if (m_bParams)
                    {
                        m_ScrollStart = m_Params[0] - 1;
                        m_ScrollEnd = m_Params[1] - 1;

                        if (m_ScrollStart >= BACKBUFFER_ROWS)
                            m_ScrollStart = BACKBUFFER_ROWS - 1;
                        if (m_ScrollEnd >= BACKBUFFER_ROWS)
                            m_ScrollEnd = BACKBUFFER_ROWS - 1;
                    }
                    else
                    {
                        m_ScrollStart = 0;
                        m_ScrollEnd = BACKBUFFER_ROWS - 1;
                    }

                    if (m_ScrollStart > m_ScrollEnd)
                    {
                        size_t tmp = m_ScrollStart;
                        m_ScrollStart = m_ScrollEnd;
                        m_ScrollEnd = tmp;
                    }

                    goHome();

                    m_bControlSeq = false;
                    break;

                case 's':
                    m_SavedCursorX = m_CursorX;
                    m_SavedCursorY = m_CursorY;
                    m_bControlSeq = false;
                    break;

                case 'u':
                    m_CursorX = m_SavedCursorX;
                    m_CursorY = m_SavedCursorY;
                    m_bControlSeq = false;
                    break;

                case 'x':
                    // Request Terminal Parameters
                    if (m_Params[0] > 1)
                    {
                        ERROR("TextIO: invalid 'sol' parameter for 'Request "
                              "Terminal Parameters'");
                    }
                    else
                    {
                        // Send back a parameter report.
                        // Parameters:
                        // * Reporting on request
                        // * No parity
                        // * 8 bits per character
                        // * 19200 bits per second xspeed
                        // * 19200 bits per second rspeed
                        // * 16x bit rate multiplier
                        // * No STP option, so no flags
                        const char *termparms = 0;
                        if (m_Params[0])
                            termparms = "\033[3;1;1;120;120;1;0x";
                        else
                            termparms = "\033[2;1;1;120;120;1;0x";
                        m_OutBuffer.write(
                            const_cast<char *>(termparms),
                            StringLength(termparms));
                    }
                    m_bControlSeq = false;
                    break;

                case 'y':
                    // Invoke Confidence Test (no-op)
                    m_bControlSeq = false;
                    break;

                default:
                    ERROR(
                        "TextIO: unknown control sequence character '"
                        << m_nCharacter << "'!");
                    m_bControlSeq = false;
                    break;
            }
        }
        else if (m_bControlSeq && (!m_bBracket) && (!m_bParenthesis))
        {
            switch (m_nCharacter)
            {
                case 0x08:
                    doBackspace();
                    break;

                case 'A':
                    if (m_CursorY > m_ScrollStart)
                        --m_CursorY;
                    m_bControlSeq = false;
                    break;

                case 'B':
                    if (m_CursorY < m_ScrollEnd)
                        ++m_CursorY;
                    m_bControlSeq = false;
                    break;

                case 'C':
                    ++m_CursorX;
                    if (m_CursorX >= m_RightMargin)
                        m_CursorX = m_RightMargin - 1;
                    m_bControlSeq = false;
                    break;

                case 'D':
                    if (m_CurrentModes & AnsiVt52)
                    {
                        // Index - cursor down one line, scroll if necessary.
                        doLinefeed();
                    }
                    else
                    {
                        // Cursor Left
                        if (m_CursorX > m_LeftMargin)
                            --m_CursorX;
                    }
                    m_bControlSeq = false;
                    break;

                case 'E':
                    // Next Line - move to start of next line.
                    doCarriageReturn();
                    doLinefeed();
                    m_bControlSeq = false;
                    break;

                case 'F':
                case 'G':
                    ERROR("TextIO: graphics mode is not implemented.");
                    m_bControlSeq = false;
                    break;

                case 'H':
                    if (m_CurrentModes & AnsiVt52)
                    {
                        // Horizontal tabulation set.
                        m_TabStops[m_CursorX] = '|';
                    }
                    else
                    {
                        // Cursor to Home
                        m_CursorX = 0;
                        m_CursorY = 0;
                    }
                    m_bControlSeq = false;
                    break;

                case 'M':
                case 'I':
                    // Reverse Index - cursor up one line, or scroll up if at
                    // top.
                    --m_CursorY;
                    checkScroll();
                    m_bControlSeq = false;
                    break;

                case 'J':
                    eraseEOS();
                    m_bControlSeq = false;
                    break;

                case 'K':
                    eraseEOL();
                    m_bControlSeq = false;
                    break;

                case 'Y':
                {
                    uint8_t row = (*(++s)) - 0x20;
                    uint8_t col = (*(++s)) - 0x20;

                    /// \todo Sanity check.
                    m_CursorX = col;
                    m_CursorY = row;
                }
                    m_bControlSeq = false;
                    break;

                case 'Z':
                {
                    const char *identifier = 0;
                    if (m_CurrentModes & AnsiVt52)
                        identifier = "\033[?1;2c";
                    else
                        identifier = "\033/Z";
                    m_OutBuffer.write(
                        const_cast<char *>(identifier),
                        StringLength(identifier));
                }
                    m_bControlSeq = false;
                    break;

                case '#':
                    // DEC commands
                    ++s;
                    m_nCharacter = *s;  /// \todo Error out if is Utf8
                    switch (m_nCharacter)
                    {
                        case '8':
                            // DEC Screen Alignment Test (DECALN)
                            // Fills screen with 'E' characters.
                            eraseScreen('E');
                            break;

                        default:
                            ERROR(
                                "TextIO: unknown DEC command '" << m_nCharacter
                                                                << "'");
                            break;
                    }
                    m_bControlSeq = false;
                    break;

                case '=':
                    /// \todo implement me!
                    ERROR("TextIO: alternate keypad mode is not implemented.");
                    m_bControlSeq = false;
                    break;

                case '<':
                    m_CurrentModes |= AnsiVt52;
                    m_bControlSeq = false;
                    break;

                case '>':
                    /// \todo implement me!
                    ERROR("TextIO: alternate keypad mode is not implemented.");
                    m_bControlSeq = false;
                    break;

                case '[':
                    m_bBracket = true;
                    break;

                case '(':
                case ')':
                case '*':
                case '+':
                case '-':
                case '.':
                case '/':
                {
                    char curr = m_nCharacter;
                    char next = *(++s);

                    // Portugese or DEC supplementary graphics (to ignore VT300
                    // command)
                    if (next == '%')
                        next = *(++s);

                    if ((next >= '0' && next <= '2') ||
                        (next >= 'A' && next <= 'B'))
                    {
                        // Designate G0 character set.
                        if (curr == '(')
                            m_G0 = next;
                        // Designate G1 character set.
                        else if (curr == ')')
                            m_G1 = next;
                        else
                            WARNING("TextIO: only 'ESC(C' and 'ESC)C' are "
                                    "supported on a VT100.");
                    }
                    m_bControlSeq = false;
                }
                break;

                case '7':
                    m_SavedCursorX = m_CursorX;
                    m_SavedCursorY = m_CursorY;
                    m_bControlSeq = false;
                    break;

                case '8':
                    m_CursorX = m_SavedCursorX;
                    m_CursorY = m_SavedCursorY;
                    m_bControlSeq = false;
                    break;

                case 'c':
                    // Power-up reset!
                    initialise(true);
                    m_bControlSeq = false;
                    break;

                default:
                    ERROR(
                        "TextIO: unknown escape sequence character '"
                        << m_nCharacter << "'!");
                    m_bControlSeq = false;
                    break;
            }
        }
        else
        {
            if (m_nCharacter == '\033')
            {
                m_bControlSeq = true;
                m_bBracket = false;
                m_bParams = false;
                m_bParenthesis = false;
                m_bQuestionMark = true;
                m_CurrentParam = 0;
                ByteSet(m_Params, 0, sizeof(size_t) * MAX_TEXTIO_PARAMS);
            }
            else
            {
                switch (m_nCharacter)
                {
                    case 0x05:
                    {
                        // Reply with our answerback.
                        const char *answerback = "\033[1;2c";
                        m_OutBuffer.write(
                            const_cast<char *>(answerback),
                            StringLength(answerback));
                    }
                    break;
                    case 0x08:
                        doBackspace();
                        break;
                    case 0x09:
                        doHorizontalTab();
                        break;
                    case '\r':
                        doCarriageReturn();
                        break;
                    case '\n':
                    case 0x0B:
                    case 0x0C:
                        if (m_CurrentModes & LineFeedNewLine)
                            doCarriageReturn();
                        doLinefeed();
                        break;
                    case 0x0E:
                        // Shift-Out - invoke G1 character set.
                        m_CurrentModes &= ~CharacterSetG0;
                        m_CurrentModes |= CharacterSetG1;
                        break;
                    case 0x0F:
                        // Shift-In - invoke G0 character set.
                        m_CurrentModes &= ~CharacterSetG1;
                        m_CurrentModes |= CharacterSetG0;
                        break;
                    default:

                        uint8_t c = translate(m_nCharacter);

                        uint8_t characterSet = m_G0;
                        if (m_CurrentModes & CharacterSetG1)
                            characterSet = m_G1;

                        if (characterSet >= '0' && characterSet <= '2')
                        {
                            switch (c)
                            {
                                case '_':
                                    c = ' ';
                                    break;  // Blank

                                // Symbols and line control.
                                case 'a':
                                    c = 0xB2;
                                    break;  // Checkerboard
                                case 'b':
                                    c = 0xAF;
                                    break;  // Horizontal tab
                                case 'c':
                                    c = 0x9F;
                                    break;  // Form feed
                                case 'h':   // Newline
                                case 'e':   // Linefeed
                                    c = 'n';
                                    break;
                                case 'i':
                                    c = 'v';
                                    break;  // Vertical tab.
                                case 'd':
                                    c = 'r';
                                    break;  // Carriage return
                                case 'f':
                                    c = 0xF8;
                                    break;  // Degree symbol
                                case 'g':
                                    c = 0xF1;
                                    break;  // Plus-minus

                                // Line-drawing.
                                case 'j':
                                    c = 0xBC;
                                    break;  // Lower right corner
                                case 'k':
                                    c = 0xBB;
                                    break;  // Upper right corner
                                case 'l':
                                    c = 0xC9;
                                    break;  // Upper left corner
                                case 'm':
                                    c = 0xC8;
                                    break;  // Lower left corner
                                case 'n':
                                    c = 0xCE;
                                    break;  // Crossing lines.
                                case 'q':
                                    c = 0xCD;
                                    break;  // Horizontal line.
                                case 't':
                                    c = 0xCC;
                                    break;  // Left 'T'
                                case 'u':
                                    c = 0xB9;
                                    break;  // Right 'T'
                                case 'v':
                                    c = 0xCA;
                                    break;  // Bottom 'T'
                                case 'w':
                                    c = 0xCB;
                                    break;  // Top 'T'
                                case 'x':
                                    c = 0xBA;
                                    break;  // Vertical bar
                            }
                        }

                        if (c >= ' ')
                        {
                            // We must handle wrapping *just before* we write
                            // the next printable, because otherwise things
                            // like BS at the right margin fail to work
                            // correctly.
                            checkWrap();

                            if (m_CursorX < BACKBUFFER_STRIDE)
                            {
                                LockGuard<Mutex> guard(m_Lock);
                                VgaCell *pCell =
                                    &m_pBackbuffer
                                        [(m_CursorY * BACKBUFFER_STRIDE) +
                                         m_CursorX];
                                pCell->character = c;
                                pCell->fore = m_Fore;
                                pCell->back = m_Back;
                                pCell->flags = m_CurrentModes;
                                ++m_CursorX;
                            }
                            else
                            {
                                ERROR(
                                    "TextIO: X co-ordinate is beyond the end "
                                    "of a backbuffer line: "
                                    << m_CursorX << " vs " << BACKBUFFER_STRIDE
                                    << "?");
                            }
                        }
                        break;
                }
            }
        }

        if (m_CursorX < m_LeftMargin)
        {
            WARNING("TextIO: X co-ordinate ended up befor the left margin.");
            m_CursorX = m_LeftMargin;
        }

        ++s;
    }

    // Assume we moved the cursor, and update where it is displayed
    // accordingly.
    if (isPrimary())
    {
        m_pVga->moveCursor(m_CursorX, m_CursorY);
    }

    // This write is now complete.
    flip();

    // Wake up anything waiting on output from us if needed.
    if (m_OutBuffer.canRead(false))
        dataChanged();
}

void TextIO::setColour(TextIO::VgaColour *which, size_t param, bool bBright)
{
    switch (param)
    {
        case 0:
            *which = bBright ? DarkGrey : Black;
            break;
        case 1:
            *which = bBright ? LightRed : Red;
            break;
        case 2:
            *which = bBright ? LightGreen : Green;
            break;
        case 3:
            *which = bBright ? Yellow : Orange;
            break;
        case 4:
            *which = bBright ? LightBlue : Blue;
            break;
        case 5:
            *which = bBright ? LightMagenta : Magenta;
            break;
        case 6:
            *which = bBright ? LightCyan : Cyan;
            break;
        case 7:
            *which = bBright ? White : LightGrey;
            break;
        default:
            break;
    }
}

void TextIO::doBackspace()
{
    // If we are at a position where we would expect to wrap, step back one
    // extra character position so we don't wrap.
    if (m_CursorX == m_RightMargin)
        --m_CursorX;

    // Backspace will not do anything if we are already on the left margin.
    if (m_CursorX > m_LeftMargin)
        --m_CursorX;
}

void TextIO::doLinefeed()
{
    ++m_CursorY;
    checkScroll();
}

void TextIO::doCarriageReturn()
{
    m_CursorX = m_LeftMargin;
}

void TextIO::doHorizontalTab()
{
    bool tabStopFound = false;

    // Move to the next tab stop from the current position.
    for (ssize_t x = (m_CursorX + 1); x < m_RightMargin; ++x)
    {
        if (m_TabStops[x] != 0)
        {
            m_CursorX = x;
            tabStopFound = true;
            break;
        }
    }

    if (!tabStopFound)
    {
        // Tab to the right margin, if no tab stop was found at all.
        m_CursorX = m_RightMargin - 1;
    }
    else if (m_CursorX >= m_RightMargin)
        m_CursorX = m_RightMargin - 1;
}

void TextIO::checkScroll()
{
    LockGuard<Mutex> guard(m_Lock);

    // Handle scrolling, which can take place due to linefeeds and
    // other such cursor movements.
    if (m_CursorY < m_ScrollStart)
    {
        // By how much have we exceeded the scroll region?
        size_t numRows = (m_ScrollStart - m_CursorY);

        // Top of the scrolling area
        size_t sourceRow = m_ScrollStart;
        size_t destRow = m_ScrollStart + numRows;

        // Bottom of the scrolling area
        size_t sourceEnd = m_ScrollEnd + 1 - numRows;

        // Move data.
        MemoryCopy(
            &m_pBackbuffer[destRow * BACKBUFFER_STRIDE],
            &m_pBackbuffer[sourceRow * BACKBUFFER_STRIDE],
            (sourceEnd - sourceRow) * BACKBUFFER_STRIDE * sizeof(VgaCell));

        // Clear out the start of the region now.
        for (size_t i = 0; i < ((destRow - sourceRow) * BACKBUFFER_STRIDE); ++i)
        {
            VgaCell *pCell =
                &m_pBackbuffer[(sourceRow * BACKBUFFER_STRIDE) + i];
            pCell->character = ' ';
            pCell->back = m_Back;
            pCell->fore = m_Fore;
            pCell->flags = 0;
        }

        m_CursorY = m_ScrollStart;
    }
    else if (m_CursorY > m_ScrollEnd)
    {
        // By how much have we exceeded the scroll region?
        size_t numRows = (m_CursorY - m_ScrollEnd);

        // At what position is the top of the scroll?
        // ie, to where are we moving the data into place?
        size_t startOffset = m_ScrollStart * BACKBUFFER_STRIDE;

        // Where are we pulling data from?
        size_t fromOffset = (m_ScrollStart + numRows) * BACKBUFFER_STRIDE;

        // How many rows are we moving? This is the distance from
        // the 'from' offset to the end of the scroll region.
        size_t movedRows = ((m_ScrollEnd + 1) * BACKBUFFER_STRIDE) - fromOffset;

        // Where do we begin blanking from?
        size_t blankFrom = (((m_ScrollEnd + 1) - numRows) * BACKBUFFER_STRIDE);

        // How much blanking do we need to do?
        size_t blankLength =
            ((m_ScrollEnd + 1) * BACKBUFFER_STRIDE) - blankFrom;

        MemoryCopy(
            &m_pBackbuffer[startOffset], &m_pBackbuffer[fromOffset],
            movedRows * sizeof(VgaCell));
        for (size_t i = 0; i < blankLength; ++i)
        {
            VgaCell *pCell = &m_pBackbuffer[blankFrom + i];
            pCell->character = ' ';
            pCell->back = m_Back;
            pCell->fore = m_Fore;
            pCell->flags = 0;
        }

        m_CursorY = m_ScrollEnd;
    }
}

void TextIO::checkWrap()
{
    if (m_CursorX >= m_RightMargin)
    {
        // Default autowrap mode is off - new characters at
        // the right margin replace any that are already there.
        if (m_CurrentModes & AutoWrap)
        {
            m_CursorX = m_LeftMargin;
            ++m_CursorY;

            checkScroll();
        }
        else
        {
            m_CursorX = m_RightMargin - 1;
        }
    }
}

void TextIO::eraseSOS()
{
    // Erase to the start of the line.
    eraseSOL();

    LockGuard<Mutex> guard(m_Lock);

    // Erase the screen above, and this line.
    for (ssize_t y = 0; y < m_CursorY; ++y)
    {
        for (size_t x = 0; x < BACKBUFFER_STRIDE; ++x)
        {
            VgaCell *pCell = &m_pBackbuffer[(y * BACKBUFFER_STRIDE) + x];
            pCell->character = ' ';
            pCell->fore = m_Fore;
            pCell->back = m_Back;
            pCell->flags = 0;
        }
    }
}

void TextIO::eraseEOS()
{
    // Erase to the end of line first...
    eraseEOL();

    LockGuard<Mutex> guard(m_Lock);

    // Then the rest of the screen.
    for (size_t y = m_CursorY + 1; y < BACKBUFFER_ROWS; ++y)
    {
        for (size_t x = 0; x < BACKBUFFER_STRIDE; ++x)
        {
            VgaCell *pCell = &m_pBackbuffer[(y * BACKBUFFER_STRIDE) + x];
            pCell->character = ' ';
            pCell->back = m_Back;
            pCell->fore = m_Fore;
            pCell->flags = 0;
        }
    }
}

void TextIO::eraseEOL()
{
    LockGuard<Mutex> guard(m_Lock);

    // Erase to end of line.
    for (size_t x = m_CursorX; x < BACKBUFFER_STRIDE; ++x)
    {
        VgaCell *pCell = &m_pBackbuffer[(m_CursorY * BACKBUFFER_STRIDE) + x];
        pCell->character = ' ';
        pCell->back = m_Back;
        pCell->fore = m_Fore;
        pCell->flags = 0;
    }
}

void TextIO::eraseSOL()
{
    LockGuard<Mutex> guard(m_Lock);

    for (ssize_t x = 0; x <= m_CursorX; ++x)
    {
        VgaCell *pCell = &m_pBackbuffer[(m_CursorY * BACKBUFFER_STRIDE) + x];
        pCell->character = ' ';
        pCell->fore = m_Fore;
        pCell->back = m_Back;
        pCell->flags = 0;
    }
}

void TextIO::eraseLine()
{
    LockGuard<Mutex> guard(m_Lock);

    for (size_t x = 0; x < BACKBUFFER_STRIDE; ++x)
    {
        VgaCell *pCell = &m_pBackbuffer[(m_CursorY * BACKBUFFER_STRIDE) + x];
        pCell->character = ' ';
        pCell->fore = m_Fore;
        pCell->back = m_Back;
        pCell->flags = 0;
    }
}

void TextIO::eraseScreen(uint8_t character)
{
    LockGuard<Mutex> guard(m_Lock);

    for (size_t y = 0; y < BACKBUFFER_ROWS; ++y)
    {
        for (size_t x = 0; x < BACKBUFFER_STRIDE; ++x)
        {
            VgaCell *pCell = &m_pBackbuffer[(y * BACKBUFFER_STRIDE) + x];
            pCell->character = character;
            pCell->fore = m_Fore;
            pCell->back = m_Back;
            pCell->flags = 0;
        }
    }
}

void TextIO::goHome(ssize_t xmove, ssize_t ymove)
{
    // Reset X/Y
    if (m_CurrentModes & Origin)
    {
        m_CursorX = m_LeftMargin + xmove;
        m_CursorY = m_ScrollStart + ymove;
    }
    else
    {
        m_CursorX = xmove;
        m_CursorY = ymove;
    }
}

void TextIO::clearBackbuffer()
{
    ByteSet(
        m_pBackbuffer, 0,
        BACKBUFFER_STRIDE * BACKBUFFER_ROWS * sizeof(VgaCell));
}

void TextIO::flip(bool timer, bool hideState)
{
    LockGuard<Mutex> guard(m_Lock);

    const VgaColour defaultBack = Black, defaultFore = LightGrey;

    // Avoid flipping if we do not have a VGA instance.
    if (!m_pVga)
        return;

    // Avoid flipping if we do not own the VGA instance.
    if (!isPrimary())
        return;

    // Avoid flipping if we aren't active.
    if (!m_bActive)
        return;

    size_t numRows = m_pVga->getNumRows();
    size_t numCols = m_pVga->getNumCols();

    for (size_t y = 0; y < numRows; ++y)
    {
        for (size_t x = 0; x < numCols; ++x)
        {
            VgaCell *pCell = &m_pBackbuffer[(y * BACKBUFFER_STRIDE) + x];
            if (timer)
            {
                if (pCell->flags & Blink)
                    pCell->hidden = hideState;
                else
                    pCell->hidden = false;  // Unhide if blink removed.
            }

            VgaColour fore = pCell->fore;
            VgaColour back = pCell->back;

            // Bold.
            if ((pCell->flags & Bright) && (fore < DarkGrey))
                fore = adjustColour(fore, true);

            if (pCell->flags & Inverse)
            {
                // Invert colours.
                VgaColour tmp = fore;
                fore = back;
                back = tmp;
            }

            uint8_t attrib = (back << 4) | (fore & 0x0F);
            if (m_CurrentModes & Screen)
            {
                // DECSCNM only applies to cells without colours.
                if (pCell->fore == defaultFore && pCell->back == defaultBack)
                {
                    attrib = (fore << 4) | (back & 0x0F);
                }
            }

            uint16_t front =
                (pCell->hidden ? ' ' : pCell->character) | (attrib << 8);
            m_pFramebuffer[(y * numCols) + x] = front;
        }
    }
}

uint64_t TextIO::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    return m_OutBuffer.read(reinterpret_cast<char *>(buffer), size, bCanBlock);
}

uint64_t TextIO::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    writeStr(reinterpret_cast<const char *>(buffer), size);
    return size;
}

int TextIO::select(bool bWriting, int timeout)
{
    if (bWriting)
    {
        return m_OutBuffer.canWrite(timeout > 0) ? 1 : 0;
    }
    else
    {
        return m_OutBuffer.canRead(timeout > 0) ? 1 : 0;
    }
}

void TextIO::flipThread()
{
    while (m_bInitialised)
    {
        bool bBlinkOn = m_NextInterval != BLINK_ON_PERIOD;
        if (bBlinkOn)
            m_NextInterval = BLINK_ON_PERIOD;
        else
            m_NextInterval = BLINK_OFF_PERIOD;

        // Flip now, triggered by the passage of time.
        flip(true, !bBlinkOn);

        // Wait for the next trigger time.
        Time::delay(m_NextInterval * Time::Multiplier::Millisecond);
    }
}

uint8_t TextIO::translate(uint32_t codepoint)
{
    // Translate codepoints into Code Page 437 representation.
    switch (codepoint)
    {
        case 0x00C7:
            return 0x80;
        case 0x00FC:
            return 0x81;
        case 0x00E9:
            return 0x82;
        case 0x00E2:
            return 0x83;
        case 0x00E4:
            return 0x84;  // ä
        case 0x00E0:
            return 0x85;
        case 0x00E5:
            return 0x86;
        case 0x00E7:
            return 0x87;
        case 0x00EA:
            return 0x88;
        case 0x00EB:
            return 0x89;
        case 0x00E8:
            return 0x8A;
        case 0x00EF:
            return 0x8B;
        case 0x00EE:
            return 0x8C;
        case 0x00EC:
            return 0x8D;
        case 0x00C4:
            return 0x8E;
        case 0x00C5:
            return 0x8F;
        case 0x00C9:
            return 0x90;
        case 0x00E6:
            return 0x91;
        case 0x00C6:
            return 0x92;
        case 0x00F4:
            return 0x93;
        case 0x00F6:
            return 0x94;
        case 0x00F2:
            return 0x95;
        case 0x00FB:
            return 0x96;
        case 0x00F9:
            return 0x97;
        case 0x00FF:
            return 0x98;
        case 0x00D6:
            return 0x99;
        case 0x00DC:
            return 0x9A;
        case 0x00A2:
            return 0x9B;
        case 0x00A3:
            return 0x9C;
        case 0x00A5:
            return 0x9D;
        case 0x20A7:
            return 0x9E;
        case 0x0192:
            return 0x9F;
        case 0x00E1:
            return 0xA0;
        case 0x00ED:
            return 0xA1;
        case 0x00F3:
            return 0xA2;
        case 0x00FA:
            return 0xA3;
        case 0x00F1:
            return 0xA4;
        case 0x00D1:
            return 0xA5;
        case 0x00AA:
            return 0xA6;
        case 0x00BA:
            return 0xA7;
        case 0x00BF:
            return 0xA8;
        case 0x2310:
            return 0xA9;
        case 0x00AC:
            return 0xAA;
        case 0x00BD:
            return 0xAB;
        case 0x00BC:
            return 0xAC;
        case 0x00A1:
            return 0xAD;
        case 0x00AB:
            return 0xAE;  // «
        case 0x00BB:
            return 0xAF;  // »
        case 0x2591:
            return 0xB0;
        case 0x2592:
            return 0xB1;
        case 0x2593:
            return 0xB2;
        case 0x2502:
            return 0xB3;
        case 0x2524:
            return 0xB4;
        case 0x2561:
            return 0xB5;
        case 0x2562:
            return 0xB6;
        case 0x2556:
            return 0xB7;
        case 0x2555:
            return 0xB8;
        case 0x2563:
            return 0xB9;
        case 0x2551:
            return 0xBA;
        case 0x2557:
            return 0xBB;
        case 0x255D:
            return 0xBC;
        case 0x255C:
            return 0xBD;
        case 0x255B:
            return 0xBE;
        case 0x2510:
            return 0xBF;
        case 0x2514:
            return 0xC0;
        case 0x2534:
            return 0xC1;
        case 0x252C:
            return 0xC2;
        case 0x251C:
            return 0xC3;
        case 0x2500:
            return 0xC4;
        case 0x253C:
            return 0xC5;
        case 0x255E:
            return 0xC6;
        case 0x255F:
            return 0xC7;
        case 0x255A:
            return 0xC8;
        case 0x2554:
            return 0xC9;
        case 0x2569:
            return 0xCA;
        case 0x2566:
            return 0xCB;
        case 0x2560:
            return 0xCC;
        case 0x2550:
            return 0xCD;
        case 0x256C:
            return 0xCE;
        case 0x2567:
            return 0xCF;
        case 0x2568:
            return 0xD0;
        case 0x2564:
            return 0xD1;
        case 0x2565:
            return 0xD2;
        case 0x2559:
            return 0xD3;
        case 0x2558:
            return 0xD4;
        case 0x2552:
            return 0xD5;
        case 0x2553:
            return 0xD6;
        case 0x256B:
            return 0xD7;
        case 0x256A:
            return 0xD8;
        case 0x2518:
            return 0xD9;
        case 0x250C:
            return 0xDA;
        case 0x2588:
            return 0xDB;
        case 0x2584:
            return 0xDC;
        case 0x258C:
            return 0xDD;
        case 0x2590:
            return 0xDE;
        case 0x2580:
            return 0xDF;
        case 0x03B1:
            return 0xE0;
        case 0x00DF:
            return 0xE1;
        case 0x0393:
            return 0xE2;
        case 0x03C0:
            return 0xE3;
        case 0x03A3:
            return 0xE4;
        case 0x03C3:
            return 0xE5;
        case 0x00B5:
            return 0xE6;
        case 0x03C4:
            return 0xE7;
        case 0x03A6:
            return 0xE8;
        case 0x0398:
            return 0xE9;
        case 0x03A9:
            return 0xEA;
        case 0x03B4:
            return 0xEB;
        case 0x221E:
            return 0xEC;
        case 0x03C6:
            return 0xED;
        case 0x03B5:
            return 0xEE;
        case 0x2229:
            return 0xEF;
        case 0x2261:
            return 0xF0;
        case 0x00B1:
            return 0xF1;
        case 0x2265:
            return 0xF2;
        case 0x2264:
            return 0xF3;
        case 0x2320:
            return 0xF4;
        case 0x2321:
            return 0xF5;
        case 0x00F7:
            return 0xF6;
        case 0x2248:
            return 0xF7;
        case 0x00B0:
            return 0xF8;
        case 0x2219:
            return 0xF9;
        case 0x00B7:
            return 0xFA;
        case 0x221A:
            return 0xFB;
        case 0x207F:
            return 0xFC;
        case 0x00B2:
            return 0xFD;
        case 0x25A0:
            return 0xFE;
        case 0x00A0:
            return 0xFF;
    }

    if (codepoint <= 0xFF)
        return codepoint & 0xFF;
    else
        return 219;  // ASCII shaded box.
}

static int startFlipThread(void *param)
{
    TextIO *tio = reinterpret_cast<TextIO *>(param);
    tio->flipThread();
    return 0;
}

void TextIO::inputCallback(InputManager::InputNotification &in)
{
    if (!in.meta)
    {
        return;
    }

    TextIO *p = reinterpret_cast<TextIO *>(in.meta);
    p->handleInput(in);
}

void TextIO::handleInput(InputManager::InputNotification &in)
{
    // Drop input if we are not the console owner.
    if (!isPrimary())
        return;

    if (!m_OutBuffer.canWrite(false))
    {
        WARNING("TextIO: output buffer is full, dropping keypress!");
        return;
    }

    if (m_InputMode == Raw)
    {
        if (in.type != InputManager::MachineKey)
        {
            return;
        }

        uint8_t buf =
            in.data.rawkey.scancode | (in.data.rawkey.keyUp ? 0x80 : 0);
        m_OutBuffer.write(reinterpret_cast<char *>(&buf), sizeof(buf));

        dataChanged();
        return;
    }

    if (in.type != InputManager::Key)
    {
        // not actually keyboard input - ignore
        return;
    }

    uint64_t c = in.data.key.key;

    int direction = -1;
    if (c & SPECIAL_KEY)
    {
        uint32_t k = c & 0xFFFFFFFFULL;
        char *str = reinterpret_cast<char *>(&k);

        if (!StringCompareN(str, "left", 4))
        {
            direction = 0;  // left
        }
        else if (!StringCompareN(str, "righ", 4))
        {
            direction = 1;  // right
        }
        else if (!StringCompareN(str, "up", 2))
        {
            direction = 2;  // up
        }
        else if (!StringCompareN(str, "down", 4))
        {
            direction = 3;  // down
        }
        else
        {
            // unhandled special key, don't send to app
            return;
        }
    }
    else if (c & CTRL_KEY)
    {
        // CTRL-key = unprintable (ie, CTRL-C, CTRL-U)
        c &= 0x1F;
    }

    if (c == '\n')
        c = '\r';  // Enter key (ie, return) - CRtoNL.

    if (direction >= 0)
    {
        switch (direction)
        {
            case 0:
                m_OutBuffer.write("\033[D", 3);
                break;
            case 1:
                m_OutBuffer.write("\033[C", 3);
                break;
            case 2:
                m_OutBuffer.write("\033[A", 3);
                break;
            case 3:
                m_OutBuffer.write("\033[B", 3);
                break;
            default:
                break;
        }
    }
    else if (c & ALT_KEY)
    {
        // ALT escaped key
        c &= 0x7F;
        char buf[2] = {'\033', static_cast<char>(c & 0xFF)};
        m_OutBuffer.write(buf, 2);
    }
    else if (c)
    {
        char buf[4];
        size_t nbuf = String::Utf32ToUtf8(c & 0xFFFFFFFF, buf);

        // UTF8 conversion complete.
        m_OutBuffer.write(buf, nbuf);
    }

    dataChanged();
}

void TextIO::markPrimary()
{
    // Set ourselves as the primary and get straight to work loading our own
    // terminal state (instead of the previous one's)
    m_bOwnsConsole = true;
    if (m_pVga)
    {
        m_pVga->moveCursor(m_CursorX, m_CursorY);
    }
    flip();
}

void TextIO::unmarkPrimary()
{
    m_bOwnsConsole = false;
}

bool TextIO::isPrimary() const
{
    return m_bOwnsConsole;
}

void TextIO::setMode(InputMode mode)
{
    m_InputMode = mode;
}

TextIO::InputMode TextIO::getMode() const
{
    return m_InputMode;
}
