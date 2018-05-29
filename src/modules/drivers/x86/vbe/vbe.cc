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

#include "VbeDisplay.h"
#include "modules/Module.h"
#include "modules/system/config/Config.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/ServiceManager.h"

#include "pedigree/kernel/machine/x86_common/Bios.h"

#ifdef CRIPPLE_HDD
#pragma GCC diagnostic ignored "-Wunreachable-code"
#endif

extern "C" void vbeModeChangedCallback(char *pId, char *pModeId);

#define REALMODE_PTR(x) ((x[1] << 4) + x[0])

static VbeDisplay *g_pDisplays[4];
static size_t g_nDisplays = 0;

struct vbeControllerInfo
{
    char signature[4];   // == "VESA"
    short version;       // == 0x0300 for VBE 3.0
    short oemString[2];  // isa vbeFarPtr
    unsigned char capabilities[4];
    unsigned short videomodes[2];  // isa vbeFarPtr
    short totalMemory;             // as # of 64KB blocks
} __attribute__((packed));

struct vbeModeInfo
{
    short attributes;
    char winA, winB;
    short granularity;
    short winsize;
    short segmentA, segmentB;
    unsigned short realFctPtr[2];
    short pitch;  // chars per scanline

    unsigned short Xres, Yres;
    unsigned char Wchar, Ychar, planes, bpp, banks;
    uint8_t memory_model, bank_size, image_pages;
    char reserved0;

    char red_mask, red_position;
    char green_mask, green_position;
    char blue_mask, blue_position;
    char rsv_mask, rsv_position;
    char directcolor_attributes;

    // --- VBE 2.0 ---
    unsigned int framebuffer;
    unsigned int offscreen;
    short sz_offscreen;  // In KB.
} __attribute__((packed));

class VbeFramebuffer : public Framebuffer
{
  public:
    VbeFramebuffer();
    VbeFramebuffer(Display *pDisplay);
    virtual ~VbeFramebuffer();

    virtual void hwRedraw(
        size_t x = ~0UL, size_t y = ~0UL, size_t w = ~0UL, size_t h = ~0UL);
    virtual void setFramebuffer(uintptr_t p);

  private:
    Display *m_pDisplay;
    char *m_pBackbuffer;
    size_t m_nBackbufferBytes;

    MemoryRegion *m_pFramebufferRegion;

    Display::ScreenMode m_Mode;
};

extern "C" void vbeModeChangedCallback(char *pId, char *pModeId)
{
    size_t id = StringToUnsignedLong(pId, 0, 10);
    size_t mode_id = StringToUnsignedLong(pModeId, 0, 10);

    if (id >= g_nDisplays)
        return;

    if (g_pDisplays[id]->getModeId() != mode_id)
    {
        g_pDisplays[id]->setScreenMode(mode_id);
    }
}

static bool entry()
{
#ifdef NOGFX
    NOTICE("Not starting VBE module, NOGFX is defined.");
    return false;
#endif

    List<Display::ScreenMode *> modeList;

    // Allocate some space for the information structure and prepare for a BIOS
    // call.
    vbeControllerInfo *info = reinterpret_cast<vbeControllerInfo *>(
        Bios::instance().malloc(/*sizeof(vbeControllerInfo)*/ 256));
    vbeModeInfo *mode = reinterpret_cast<vbeModeInfo *>(
        Bios::instance().malloc(/*sizeof(vbeModeInfo)*/ 256));
    QuadWordSet(info, 0, 256 / 8);
    QuadWordSet(mode, 0, 256 / 8);
    StringCopyN(info->signature, "VBE2", 4);
    Bios::instance().setAx(0x4F00);
    Bios::instance().setEs(0x0000);
    Bios::instance().setDi(
        static_cast<uint16_t>(reinterpret_cast<uintptr_t>(info) & 0xFFFF));

    uint16_t ax = Bios::instance().getAx();
    uint16_t bx = Bios::instance().getBx();
    uint16_t cx = Bios::instance().getCx();
    uint16_t dx = Bios::instance().getDx();
    uint16_t di = Bios::instance().getDi();
    NOTICE("abcd: " << Hex << ax << ", " << bx << ", " << cx << ", " << dx);
    NOTICE("di: " << Hex << di);

    Bios::instance().executeInterrupt(0x10);

    // Check the signature.
    ax = Bios::instance().getAx();
    bx = Bios::instance().getBx();
    cx = Bios::instance().getCx();
    dx = Bios::instance().getDx();
    di = Bios::instance().getDi();
    if (ax != 0x004F || StringCompareN(info->signature, "VESA", 4) != 0)
    {
        ERROR(
            "VBE: VESA not supported (ax=" << Hex << ax << ", signature="
                                           << info->signature << ")!");
        NOTICE("abcd: " << Hex << ax << ", " << bx << ", " << cx << ", " << dx);
        NOTICE("di: " << Hex << di);
        Bios::instance().free(reinterpret_cast<uintptr_t>(info));
        Bios::instance().free(reinterpret_cast<uintptr_t>(mode));
        return false;
    }

    VbeDisplay::VbeVersion vbeVersion;
    switch (info->version)
    {
        case 0x0102:
            vbeVersion = VbeDisplay::Vbe1_2;
            break;
        case 0x0200:
            vbeVersion = VbeDisplay::Vbe2_0;
            break;
        case 0x0300:
            vbeVersion = VbeDisplay::Vbe3_0;
            break;
        default:
            ERROR("VBE: Unrecognised VESA version: " << Hex << info->version);
            Bios::instance().free(reinterpret_cast<uintptr_t>(info));
            Bios::instance().free(reinterpret_cast<uintptr_t>(mode));
            return false;
    }

    size_t maxWidth = 0;
    size_t maxHeight = 0;
    size_t maxBpp = 0;

    size_t maxTextWidth = 0;
    size_t maxTextHeight = 0;

    uintptr_t fbAddr = 0;
    uint16_t *modes =
        reinterpret_cast<uint16_t *>(REALMODE_PTR(info->videomodes));
    for (int i = 0; modes[i] != 0xFFFF; i++)
    {
        Bios::instance().setAx(0x4F01);
        Bios::instance().setCx(modes[i]);
        Bios::instance().setEs(0x0000);
        Bios::instance().setDi(
            static_cast<uint16_t>(reinterpret_cast<uintptr_t>(mode) & 0xFFFF));

        Bios::instance().executeInterrupt(0x10);

        ax = Bios::instance().getAx();
        if (ax != 0x004F)
        {
            WARNING(
                "Testing for mode " << Hex << modes[i] << " failed, ax=" << ax);
            continue;
        }

        // graphics/text mode bit
        bool isGraphicsMode = mode->attributes & 0x10;
        bool hasLFB = mode->attributes & 0x80;

        if (isGraphicsMode)
        {
            // We only want graphics modes with LFB support.
            if (!hasLFB)
            {
                continue;
            }
            // Check if this is a packed pixel or direct colour mode.
            else if (mode->memory_model != 4 && mode->memory_model != 6)
            {
                continue;
            }
        }

        // Add this pixel mode.
        Display::ScreenMode *pSm = new Display::ScreenMode;
        pSm->id = modes[i];
        pSm->width = mode->Xres;
        pSm->height = mode->Yres;
        pSm->refresh = 0;
        pSm->framebuffer = mode->framebuffer;
        pSm->textMode = !isGraphicsMode;
        fbAddr = mode->framebuffer;
        pSm->pf.mRed = mode->red_mask;
        pSm->pf.pRed = mode->red_position;
        pSm->pf.mGreen = mode->green_mask;
        pSm->pf.pGreen = mode->green_position;
        pSm->pf.mBlue = mode->blue_mask;
        pSm->pf.pBlue = mode->blue_position;
        pSm->pf.nBpp = mode->bpp;
        pSm->pf.nPitch = mode->pitch;
        modeList.pushBack(pSm);

        if (isGraphicsMode)
        {
            if (mode->Xres > maxWidth)
                maxWidth = mode->Xres;
            if (mode->Yres > maxHeight)
                maxHeight = mode->Yres;
        }
        else
        {
            if (mode->Xres > maxTextWidth)
                maxTextWidth = mode->Xres;
            if (mode->Yres > maxTextHeight)
                maxTextHeight = mode->Yres;
        }
        if (mode->bpp > maxBpp)
            maxBpp = mode->bpp;
    }

    // Total video memory, in bytes.
    size_t totalMemory = info->totalMemory * 64 * 1024;

    Bios::instance().free(reinterpret_cast<uintptr_t>(info));
    Bios::instance().free(reinterpret_cast<uintptr_t>(mode));

    NOTICE("VBE: Detected compatible display modes:");

    for (List<Display::ScreenMode *>::Iterator it = modeList.begin();
         it != modeList.end(); it++)
    {
        Display::ScreenMode *pSm = *it;
        NOTICE(
            Hex << pSm->id << "\t " << Dec << pSm->width << "x" << pSm->height
                << "x" << pSm->pf.nBpp << "\t " << Hex << pSm->framebuffer);
        if (!pSm->textMode)
        {
            NOTICE(
                "    " << pSm->pf.mRed << "<<" << pSm->pf.pRed << "    "
                       << pSm->pf.mGreen << "<<" << pSm->pf.pGreen << "    "
                       << pSm->pf.mBlue << "<<" << pSm->pf.pBlue);
        }
        else
        {
            NOTICE("    text mode");
        }
    }
    NOTICE("VBE: End of compatible display modes.");

    // Now that we have a framebuffer address, we can (hopefully) find the
    // device in the device tree that owns that address.
    Device *pDevice = 0;
    auto searchNode = [&pDevice, fbAddr](Device *pDev) {
        if (pDevice)
            return pDev;

        // Get its addresses, and search for fbAddr.
        for (unsigned int j = 0; j < pDev->addresses().count(); j++)
        {
            if (pDev->getPciClassCode() == 0x03 &&
                pDev->addresses()[j]->m_Address <= fbAddr &&
                (pDev->addresses()[j]->m_Address +
                 pDev->addresses()[j]->m_Size) > fbAddr)
            {
                pDevice = pDev;
                break;
            }
        }

        return pDev;
    };
    auto f = pedigree_std::make_callable(searchNode);
    Device::foreach (f, 0);
    if (!pDevice)
    {
        ERROR(
            "VBE: Device mapped to framebuffer address '" << Hex << fbAddr
                                                          << "' not found.");
        return false;
    }

    // Create a new VbeDisplay device node.
    VbeDisplay *pDisplay =
        new VbeDisplay(pDevice, vbeVersion, modeList, totalMemory, g_nDisplays);

    g_pDisplays[g_nDisplays] = pDisplay;

    // Does the display already exist in the database?
    bool bDelayedInsert = false;
    size_t mode_id = 0;
    String str;
    str.Format(
        "SELECT * FROM displays WHERE pointer=%d",
        reinterpret_cast<uintptr_t>(pDisplay));
    Config::Result *pResult = Config::instance().query(str);
    if (!pResult)
    {
        ERROR("vbe: Got no result when selecting displays");
    }
    else if (pResult->succeeded() && pResult->rows() == 1)
    {
        mode_id = pResult->getNum(0, "mode_id");
        delete pResult;
        str.Format(
            "UPDATE displays SET id=%d WHERE pointer=%d", g_nDisplays,
            reinterpret_cast<uintptr_t>(pDisplay));
        pResult = Config::instance().query(str);
        if (!pResult->succeeded())
            FATAL("Display update failed: " << pResult->errorMessage());
        delete pResult;
    }
    else if (pResult->succeeded() && pResult->rows() > 1)
    {
        delete pResult;
        FATAL(
            "Multiple displays for pointer `"
            << reinterpret_cast<uintptr_t>(pDisplay) << "'");
    }
    else if (pResult->succeeded())
    {
        delete pResult;
        bDelayedInsert = true;
    }
    else
    {
        FATAL("Display select failed: " << pResult->errorMessage());
        delete pResult;
    }

    g_nDisplays++;

    VbeFramebuffer *pFramebuffer = new VbeFramebuffer(pDisplay);
    pDisplay->setLogicalFramebuffer(pFramebuffer);

    GraphicsService::GraphicsProvider *pProvider =
        new GraphicsService::GraphicsProvider;
    pProvider->pDisplay = pDisplay;
    pProvider->pFramebuffer = pFramebuffer;
    pProvider->maxWidth = maxWidth;
    pProvider->maxHeight = maxHeight;
    pProvider->maxTextWidth = maxTextWidth;
    pProvider->maxTextHeight = maxTextHeight;
    pProvider->maxDepth = maxBpp;
    pProvider->bHardwareAccel = false;
    pProvider->bTextModes = true;

    // Register with the graphics service
    ServiceFeatures *pFeatures =
        ServiceManager::instance().enumerateOperations(String("graphics"));
    Service *pService =
        ServiceManager::instance().getService(String("graphics"));
    if (pFeatures->provides(ServiceFeatures::touch))
        if (pService)
            pService->serve(
                ServiceFeatures::touch, reinterpret_cast<void *>(pProvider),
                sizeof(*pProvider));

    // Replace pDev with pDisplay.
    pDisplay->setParent(pDevice->getParent());
    pDevice->getParent()->replaceChild(pDevice, pDisplay);

    return true;
}

static void exit()
{
}

MODULE_INFO("vbe", &entry, &exit, "pci", "config");

VbeFramebuffer::VbeFramebuffer()
    : Framebuffer(), m_pDisplay(0), m_pBackbuffer(0), m_nBackbufferBytes(0),
      m_pFramebufferRegion(0), m_Mode()
{
}

VbeFramebuffer::VbeFramebuffer(Display *pDisplay)
    : Framebuffer(), m_pDisplay(pDisplay), m_pBackbuffer(0),
      m_nBackbufferBytes(0), m_pFramebufferRegion(0), m_Mode()
{
}

void VbeFramebuffer::hwRedraw(size_t x, size_t y, size_t w, size_t h)
{
    if (x == ~0UL)
        x = 0;
    if (y == ~0UL)
        y = 0;
    if (w == ~0UL)
        w = m_Mode.width;
    if (h == ~0UL)
        h = m_Mode.height;

    if (x == 0 && y == 0 && w >= m_Mode.width && h >= m_Mode.height)
    {
        // Full-screen refresh.
        MemoryCopy(
            m_pDisplay->getFramebuffer(), m_pBackbuffer, m_nBackbufferBytes);
        return;
    }

    // We have a smaller copy than the entire screen.
    size_t bytesPerRow = w * m_Mode.bytesPerPixel;
    size_t xOffset = x * m_Mode.bytesPerPixel;
    size_t yOffset = y * m_Mode.bytesPerLine;

    void *firstRowTarget =
        adjust_pointer(m_pDisplay->getFramebuffer(), yOffset + xOffset);
    void *firstRowBackbuffer = adjust_pointer(m_pBackbuffer, yOffset + xOffset);
    for (size_t yy = 0; yy < h; ++yy)
    {
        void *targetRow =
            adjust_pointer(firstRowTarget, yy * m_Mode.bytesPerLine);
        void *backbufferRow =
            adjust_pointer(firstRowBackbuffer, yy * m_Mode.bytesPerLine);
        MemoryCopy(targetRow, backbufferRow, bytesPerRow);
    }
}

VbeFramebuffer::~VbeFramebuffer()
{
}

void VbeFramebuffer::setFramebuffer(uintptr_t p)
{
    ByteSet(&m_Mode, 0, sizeof(Display::ScreenMode));
    if (!m_pDisplay->getCurrentScreenMode(m_Mode))
    {
        ERROR("VBE: setting screen mode failed.");
        return;
    }
    m_nBackbufferBytes = m_Mode.bytesPerLine * m_Mode.height;
    if (m_nBackbufferBytes)
    {
        m_pBackbuffer = 0;

        if (m_pFramebufferRegion)
        {
            delete m_pFramebufferRegion;
        }

        size_t nPages = (m_nBackbufferBytes +
                         PhysicalMemoryManager::instance().getPageSize()) /
                        PhysicalMemoryManager::instance().getPageSize();

        m_pFramebufferRegion = new MemoryRegion("VBE Backbuffer");
        if (!PhysicalMemoryManager::instance().allocateRegion(
                *m_pFramebufferRegion, nPages,
                PhysicalMemoryManager::continuous, VirtualAddressSpace::Write))
        {
            delete m_pFramebufferRegion;
            m_pFramebufferRegion = 0;
        }
        else
        {
            NOTICE(
                "VBE backbuffer is at " << reinterpret_cast<uintptr_t>(
                    m_pFramebufferRegion->virtualAddress()));
            m_pBackbuffer = reinterpret_cast<char *>(
                m_pFramebufferRegion->virtualAddress());
        }

        Framebuffer::setFramebuffer(reinterpret_cast<uintptr_t>(m_pBackbuffer));
    }
}
