/*
 * Copyright (c) 2008-2026, Pedigree Developers
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

#include "VmwareSvgaState.h"
#include "modules/Module.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Vga.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"
#include "svga_reg.h"
#include "vm_device_version.h"

namespace
{
struct Mode
{
    size_t id;
    size_t width;
    size_t height;
    Graphics::PixelFormat format;
};

Mode g_Modes[] = {
    {0x117, 1024, 768, Graphics::Bits16_Rgb555},
    {0, 80, 25, Graphics::Bits8_Idx},
};

constexpr size_t ModeCount = sizeof(g_Modes) / sizeof(g_Modes[0]);
constexpr size_t LargestCommandWords = 7;
constexpr size_t MinimumFifoBytes = 10 * 1024;
}  // namespace

class VmwareGraphics : public Display
{
  public:
    explicit VmwareGraphics(Device *pDev)
        : Display(pDev), m_pIo(nullptr), m_Framebuffer(nullptr),
          m_CommandRegion(nullptr), m_pFramebuffer(nullptr),
          m_pProvider(nullptr), m_DeviceLock(), m_Capabilities(0),
          m_MaxWidth(0), m_MaxHeight(0), m_FramebufferSize(0),
          m_CommandRegionSize(0), m_FifoHeaderBytes(0),
          m_HardwareAccepted(false), m_Online(false), m_FifoRunning(false),
          m_Stopping(false), m_ProviderRegistered(false),
          m_ShutdownComplete(false)
    {
    }

    virtual ~VmwareGraphics();

    void shutdown();

    bool initialise()
    {
        {
            LockGuard<Mutex> guard(m_DeviceLock);
            if (!initialiseHardwareLocked())
            {
                disableHardwareLocked();
                return false;
            }

            m_pFramebuffer = new VmwareFramebuffer(0, this);
            m_pProvider = new GraphicsService::GraphicsProvider;
            if (!m_pFramebuffer || !m_pProvider)
            {
                markOfflineLocked("could not allocate provider state");
                return false;
            }

            m_pProvider->pDisplay = this;
            m_pProvider->pFramebuffer = m_pFramebuffer;
            m_pProvider->maxWidth = m_MaxWidth;
            m_pProvider->maxHeight = m_MaxHeight;
            m_pProvider->maxTextWidth = 0;
            m_pProvider->maxTextHeight = 0;
            m_pProvider->maxDepth = 32;
            m_pProvider->bHardwareAccel = m_Capabilities & SVGA_CAP_RECT_COPY;
            m_pProvider->bTextModes = false;
            m_Online = true;
        }

        if (!registerProvider())
        {
            LockGuard<Mutex> guard(m_DeviceLock);
            markOfflineLocked("could not register with the graphics service");
            return false;
        }

        return true;
    }

    virtual void getName(String &str)
    {
        str.assign("vmware-gfx", 11);
    }

    virtual void dump(String &str)
    {
        str.assign("vmware guest tools, graphics card", 34);
    }

    virtual bool getCurrentScreenMode(Display::ScreenMode &sm)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked())
            return false;

        sm.width = readRegisterLocked(SVGA_REG_WIDTH);
        sm.height = readRegisterLocked(SVGA_REG_HEIGHT);
        sm.pf.nBpp = readRegisterLocked(SVGA_REG_BITS_PER_PIXEL);

        const uint32_t redMask = readRegisterLocked(SVGA_REG_RED_MASK);
        const uint32_t greenMask = readRegisterLocked(SVGA_REG_GREEN_MASK);
        const uint32_t blueMask = readRegisterLocked(SVGA_REG_BLUE_MASK);
        sm.pf2 = pixelFormat(sm.pf.nBpp, redMask, greenMask, blueMask);
        sm.bytesPerPixel = (sm.pf.nBpp + 7) / 8;
        sm.bytesPerLine = readRegisterLocked(SVGA_REG_BYTES_PER_LINE);
        return true;
    }

    virtual bool getScreenModes(List<Display::ScreenMode *> &sms)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked())
            return false;

        for (size_t i = 0; i < ModeCount; ++i)
        {
            Display::ScreenMode *pMode = new Display::ScreenMode;
            pMode->id = g_Modes[i].id;
            pMode->width = g_Modes[i].width;
            pMode->height = g_Modes[i].height;
            pMode->pf2 = g_Modes[i].format;
            sms.pushBack(pMode);
        }

        return true;
    }

    virtual bool setScreenMode(Display::ScreenMode sm)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked())
            return false;

        if (!sm.id)
        {
            disableDisplayLocked();
            return true;
        }

        if (!setModeLocked(sm.width, sm.height, Graphics::bitsPerPixel(sm.pf2)))
            return false;

        Vga *pVga = Machine::instance().getVga(0);
        if (pVga)
            pVga->setMode(sm.id);
        return true;
    }

    virtual bool setScreenMode(size_t modeId)
    {
        // panic() disables interrupts before asking the active display to
        // return to mode zero. Never wait on a mutex in that path.
        if (!modeId && !Processor::getInterrupts())
        {
            if (!m_DeviceLock.tryAcquire())
                return false;
            const bool available = availableLocked();
            if (available)
                disableDisplayLocked();
            m_DeviceLock.release();
            return available;
        }

        if (!modeId)
        {
            LockGuard<Mutex> guard(m_DeviceLock);
            if (!availableLocked())
                return false;
            disableDisplayLocked();
            return true;
        }

        return Display::setScreenMode(modeId);
    }

    virtual bool setScreenMode(size_t width, size_t height, size_t bpp)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked())
            return false;

        if (width > m_MaxWidth)
            width = m_MaxWidth;
        if (height > m_MaxHeight)
            height = m_MaxHeight;
        return setModeLocked(width, height, bpp);
    }

    bool redraw(size_t x, size_t y, size_t width, size_t height)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked() || !m_FifoRunning || !m_pFramebuffer)
            return false;

        const size_t screenWidth = m_pFramebuffer->getWidth();
        const size_t screenHeight = m_pFramebuffer->getHeight();
        if ((x >= screenWidth) || (y >= screenHeight))
            return true;
        if (width > (screenWidth - x))
            width = screenWidth - x;
        if (height > (screenHeight - y))
            height = screenHeight - y;
        if (!width || !height)
            return true;

        const uint32_t command[] = {
            SVGA_CMD_UPDATE, static_cast<uint32_t>(x), static_cast<uint32_t>(y),
            static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        return submitFifoLocked(command, sizeof(command) / sizeof(command[0]));
    }

    bool copy(
        size_t sourceX, size_t sourceY, size_t destinationX,
        size_t destinationY, size_t width, size_t height, bool softwareFallback)
    {
        if (!Processor::getInterrupts())
            return false;
        LockGuard<Mutex> guard(m_DeviceLock);
        if (!availableLocked() || !m_pFramebuffer ||
            !m_pFramebuffer->getActive())
            return false;

        const size_t screenWidth = m_pFramebuffer->getWidth();
        const size_t screenHeight = m_pFramebuffer->getHeight();
        if ((sourceX >= screenWidth) || (destinationX >= screenWidth) ||
            (sourceY >= screenHeight) || (destinationY >= screenHeight))
            return false;

        if (width > (screenWidth - sourceX))
            width = screenWidth - sourceX;
        if (width > (screenWidth - destinationX))
            width = screenWidth - destinationX;
        if (height > (screenHeight - sourceY))
            height = screenHeight - sourceY;
        if (height > (screenHeight - destinationY))
            height = screenHeight - destinationY;
        if (!width || !height)
            return true;

        if (m_FifoRunning && (m_Capabilities & SVGA_CAP_RECT_COPY))
        {
            const uint32_t command[] = {
                SVGA_CMD_RECT_COPY,
                static_cast<uint32_t>(sourceX),
                static_cast<uint32_t>(sourceY),
                static_cast<uint32_t>(destinationX),
                static_cast<uint32_t>(destinationY),
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)};
            if (submitFifoLocked(command, sizeof(command) / sizeof(command[0])))
                return true;
            if (!availableLocked())
                return false;
        }

        if (!softwareFallback)
            return false;
        m_pFramebuffer->softwareCopy(
            sourceX, sourceY, destinationX, destinationY, width, height);
        return true;
    }

    class VmwareFramebuffer : public Framebuffer
    {
      public:
        VmwareFramebuffer(uintptr_t framebuffer, VmwareGraphics *pDisplay)
            : Framebuffer(), m_CallbackOperations(), m_pDisplay(pDisplay)
        {
            setFramebuffer(framebuffer);
        }

        virtual ~VmwareFramebuffer() = default;

        void closeCallbacks()
        {
            m_CallbackOperations.close();
        }

        void detach()
        {
            m_CallbackOperations.wait();
            m_pDisplay = nullptr;
            setActive(false);
            setFramebuffer(0);
        }

        virtual void hwRedraw(
            size_t x = ~0UL, size_t y = ~0UL, size_t width = ~0UL,
            size_t height = ~0UL)
        {
            if (!Processor::getInterrupts())
                return;
            OperationBarrier::Lease callback;
            if (!m_CallbackOperations.tryAcquire(callback))
                return;
            if (!m_pDisplay)
                return;
            if (x == ~0UL)
                x = 0;
            if (y == ~0UL)
                y = 0;
            m_pDisplay->redraw(x, y, width, height);
        }

        virtual void copy(
            size_t sourceX, size_t sourceY, size_t destinationX,
            size_t destinationY, size_t width, size_t height,
            bool bLowestCall = true)
        {
            if (!Processor::getInterrupts())
                return;
            OperationBarrier::Lease callback;
            if (!m_CallbackOperations.tryAcquire(callback))
                return;
            if (m_pDisplay)
                m_pDisplay->copy(
                    sourceX, sourceY, destinationX, destinationY, width, height,
                    bLowestCall);
        }

        void softwareCopy(
            size_t sourceX, size_t sourceY, size_t destinationX,
            size_t destinationY, size_t width, size_t height)
        {
            swCopy(sourceX, sourceY, destinationX, destinationY, width, height);
        }

      private:
        OperationBarrier m_CallbackOperations;
        VmwareGraphics *m_pDisplay;
    };

  private:
    bool availableLocked() const
    {
        return m_Online && !m_Stopping;
    }

    static Graphics::PixelFormat pixelFormat(
        size_t bpp, uint32_t redMask, uint32_t greenMask, uint32_t blueMask)
    {
        switch (bpp)
        {
            case 24:
                return redMask > blueMask ? Graphics::Bits24_Rgb :
                                            Graphics::Bits24_Bgr;
            case 16:
                if ((redMask == greenMask) && (greenMask == blueMask))
                {
                    return blueMask == 0xF ? Graphics::Bits16_Argb :
                                             Graphics::Bits16_Rgb555;
                }
                return Graphics::Bits16_Rgb565;
            default:
                return Graphics::Bits32_Argb;
        }
    }

    bool initialiseHardwareLocked()
    {
        if (m_Addresses.count() < 3)
        {
            WARNING("vmware-gfx has an incomplete PCI BAR set");
            return false;
        }

        if (!m_Addresses[0])
        {
            WARNING("vmware-gfx has no register I/O BAR");
            return false;
        }

        m_pIo = m_Addresses[0]->m_Io;
        if (!m_pIo || !(*m_pIo))
        {
            WARNING("vmware-gfx has no usable register I/O BAR");
            return false;
        }

        writeRegisterLocked(SVGA_REG_ID, SVGA_MAKE_ID(2));
        if (readRegisterLocked(SVGA_REG_ID) != SVGA_MAKE_ID(2))
        {
            WARNING("vmware-gfx is not a compatible SVGA2 device");
            return false;
        }
        m_HardwareAccepted = true;

        const uintptr_t fbBase = readRegisterLocked(SVGA_REG_FB_START);
        const size_t vramSize = readRegisterLocked(SVGA_REG_VRAM_SIZE);
        const size_t framebufferSize = readRegisterLocked(SVGA_REG_FB_SIZE);
        const uintptr_t commandBase = readRegisterLocked(SVGA_REG_MEM_START);
        const size_t commandSize = readRegisterLocked(SVGA_REG_MEM_SIZE);
        m_Capabilities = readRegisterLocked(SVGA_REG_CAPABILITIES);
        m_MaxWidth = readRegisterLocked(SVGA_REG_MAX_WIDTH);
        m_MaxHeight = readRegisterLocked(SVGA_REG_MAX_HEIGHT);

        Device::Address *pFramebufferAddress = nullptr;
        Device::Address *pCommandAddress = nullptr;
        for (size_t i = 1; i < m_Addresses.count(); ++i)
        {
            Device::Address *pAddress = m_Addresses[i];
            if (!pAddress || pAddress->m_IsIoSpace)
                continue;
            if (pAddress->m_Address == fbBase)
                pFramebufferAddress = pAddress;
            if (pAddress->m_Address == commandBase)
                pCommandAddress = pAddress;
        }

        if (!pFramebufferAddress || !pCommandAddress ||
            (pFramebufferAddress == pCommandAddress) ||
            !pFramebufferAddress->m_Io || !pCommandAddress->m_Io ||
            !m_MaxWidth || !m_MaxHeight || !commandSize ||
            (commandSize > pCommandAddress->m_Size) ||
            (commandSize > static_cast<size_t>(~static_cast<uint32_t>(0))))
        {
            WARNING("vmware-gfx reported an invalid framebuffer/FIFO layout");
            return false;
        }

        m_Framebuffer =
            static_cast<MemoryMappedIo *>(pFramebufferAddress->m_Io);
        m_CommandRegion = static_cast<MemoryMappedIo *>(pCommandAddress->m_Io);
        m_FramebufferSize = pFramebufferAddress->m_Size;
        if (framebufferSize && (framebufferSize < m_FramebufferSize))
            m_FramebufferSize = framebufferSize;
        m_CommandRegionSize = commandSize;
        if (!m_FramebufferSize)
        {
            WARNING("vmware-gfx reported an empty framebuffer aperture");
            return false;
        }

        // Address::map() is one-shot, so map the complete usable aperture now
        // rather than assuming a later, larger mode can grow the mapping.
        pFramebufferAddress->map(m_FramebufferSize, true, true);
        if (!m_Framebuffer->virtualAddress() ||
            (m_Framebuffer->size() < m_FramebufferSize))
        {
            WARNING("vmware-gfx could not map its framebuffer aperture");
            return false;
        }

        pCommandAddress->map(m_CommandRegionSize);
        if (!m_CommandRegion->virtualAddress() ||
            (m_CommandRegion->size() < m_CommandRegionSize))
        {
            WARNING("vmware-gfx could not map its FIFO aperture");
            return false;
        }

        uint32_t fifoMin = 4 * sizeof(uint32_t);
        const uint32_t fifoMax =
            static_cast<uint32_t>(commandSize) & ~static_cast<uint32_t>(3);
        if (m_Capabilities & SVGA_CAP_EXTENDED_FIFO)
        {
            const size_t fifoRegisters = readRegisterLocked(SVGA_REG_MEM_REGS);
            if ((fifoRegisters < SVGA_FIFO_NUM_REGS) ||
                (fifoRegisters >
                 (static_cast<size_t>(~static_cast<uint32_t>(0)) /
                  sizeof(uint32_t))))
            {
                WARNING("vmware-gfx reported an invalid extended FIFO header");
                return false;
            }
            fifoMin = static_cast<uint32_t>(fifoRegisters * sizeof(uint32_t));
        }

        if ((fifoMax <= fifoMin) || ((fifoMax - fifoMin) < MinimumFifoBytes))
        {
            WARNING("vmware-gfx FIFO is too small for supported commands");
            return false;
        }
        m_FifoHeaderBytes = fifoMin;

        disableDisplayLocked();
        volatile uint32_t *fifo = fifoLocked();
        fifo[SVGA_FIFO_MIN] = fifoMin;
        fifo[SVGA_FIFO_MAX] = fifoMax;
        fifo[SVGA_FIFO_NEXT_CMD] = fifoMin;
        fifo[SVGA_FIFO_STOP] = fifoMin;
        asm volatile("" : : : "memory");

        writeRegisterLocked(SVGA_REG_GUEST_ID, 0x500A);
        NOTICE(
            "vmware-gfx found, caps="
            << Hex << m_Capabilities << ", maximum resolution is " << Dec
            << m_MaxWidth << "x" << m_MaxHeight << Hex);
        NOTICE(
            "vmware-gfx framebuffer at "
            << Hex << fbBase << " - " << (fbBase + vramSize)
            << ", command FIFO at " << commandBase);
        if (m_Capabilities & SVGA_CAP_EXTENDED_FIFO)
        {
            NOTICE(
                "vmware-gfx using extended fifo, caps="
                << fifo[SVGA_FIFO_CAPABILITIES]
                << ", flags=" << fifo[SVGA_FIFO_FLAGS]);
        }

        return true;
    }

    bool registerProvider()
    {
        ServiceFeatures *pFeatures =
            ServiceManager::instance().enumerateOperations(String("graphics"));
        Service *pService =
            ServiceManager::instance().getService(String("graphics"));
        if (!pFeatures || !pService ||
            !pFeatures->provides(ServiceFeatures::touch) ||
            !pService->serve(
                ServiceFeatures::touch, static_cast<void *>(m_pProvider),
                sizeof(*m_pProvider)))
            return false;

        LockGuard<Mutex> guard(m_DeviceLock);
        m_ProviderRegistered = true;
        return true;
    }

    void unregisterProvider()
    {
        GraphicsService::GraphicsProvider *pProvider = nullptr;
        {
            LockGuard<Mutex> guard(m_DeviceLock);
            if (!m_ProviderRegistered)
                return;
            pProvider = m_pProvider;
        }

        ServiceFeatures *pFeatures =
            ServiceManager::instance().enumerateOperations(String("graphics"));
        Service *pService =
            ServiceManager::instance().getService(String("graphics"));
        if (!pFeatures || !pService ||
            !pFeatures->provides(ServiceFeatures::withdraw) ||
            !pService->serve(
                ServiceFeatures::withdraw, static_cast<void *>(pProvider),
                sizeof(*pProvider)))
        {
            panic("vmware-gfx could not withdraw its graphics provider");
        }

        LockGuard<Mutex> guard(m_DeviceLock);
        m_ProviderRegistered = false;
    }

    bool setModeLocked(size_t width, size_t height, size_t bpp)
    {
        if (!availableLocked() || !width || !height || !bpp ||
            (width > m_MaxWidth) || (height > m_MaxHeight) || (bpp > 32))
            return false;

        if (m_FifoRunning && !syncFifoLocked())
        {
            markOfflineLocked("timed out draining FIFO before a mode change");
            return false;
        }

        writeRegisterLocked(SVGA_REG_CONFIG_DONE, 0);
        m_FifoRunning = false;
        writeRegisterLocked(SVGA_REG_ENABLE, 1);
        writeRegisterLocked(SVGA_REG_WIDTH, static_cast<uint32_t>(width));
        writeRegisterLocked(SVGA_REG_HEIGHT, static_cast<uint32_t>(height));
        writeRegisterLocked(
            SVGA_REG_BITS_PER_PIXEL, static_cast<uint32_t>(bpp));

        const size_t actualWidth = readRegisterLocked(SVGA_REG_WIDTH);
        const size_t actualHeight = readRegisterLocked(SVGA_REG_HEIGHT);
        const size_t actualDepth = readRegisterLocked(SVGA_REG_DEPTH);
        const size_t fbOffset = readRegisterLocked(SVGA_REG_FB_OFFSET);
        const uintptr_t fbBase = readRegisterLocked(SVGA_REG_FB_START);
        const uint32_t redMask = readRegisterLocked(SVGA_REG_RED_MASK);
        const uint32_t greenMask = readRegisterLocked(SVGA_REG_GREEN_MASK);
        const uint32_t blueMask = readRegisterLocked(SVGA_REG_BLUE_MASK);
        const size_t bytesPerLine = readRegisterLocked(SVGA_REG_BYTES_PER_LINE);

        if (!actualWidth || !actualHeight || !bytesPerLine ||
            (actualWidth > m_MaxWidth) || (actualHeight > m_MaxHeight) ||
            (actualWidth >
             ((~static_cast<size_t>(0) - 7) / static_cast<size_t>(bpp))) ||
            (bytesPerLine < ((actualWidth * bpp + 7) / 8)) ||
            (actualHeight > (~static_cast<size_t>(0) / bytesPerLine)))
        {
            markOfflineLocked("device returned an invalid mode layout");
            return false;
        }

        const size_t framebufferBytes = actualHeight * bytesPerLine;
        if ((fbOffset > m_FramebufferSize) ||
            (framebufferBytes > (m_FramebufferSize - fbOffset)))
        {
            markOfflineLocked("mode exceeds the framebuffer aperture");
            return false;
        }

        const size_t requiredMapping = fbOffset + framebufferBytes;
        if (!m_Framebuffer->virtualAddress() ||
            (m_Framebuffer->size() < requiredMapping))
        {
            markOfflineLocked("could not map the complete framebuffer mode");
            return false;
        }

        m_pFramebuffer->setWidth(actualWidth);
        m_pFramebuffer->setHeight(actualHeight);
        m_pFramebuffer->setBytesPerPixel((bpp + 7) / 8);
        m_pFramebuffer->setBytesPerLine(bytesPerLine);
        m_pFramebuffer->setFormat(
            pixelFormat(bpp, redMask, greenMask, blueMask));
        m_pFramebuffer->setXPos(0);
        m_pFramebuffer->setYPos(0);
        m_pFramebuffer->setParent(nullptr);
        m_pFramebuffer->setFramebuffer(
            reinterpret_cast<uintptr_t>(m_Framebuffer->virtualAddress()) +
            fbOffset);
        m_pFramebuffer->setActive(true);
        m_pFramebuffer->rect(0, 0, actualWidth, actualHeight, 0);

        asm volatile("" : : : "memory");
        writeRegisterLocked(SVGA_REG_CONFIG_DONE, 1);
        m_FifoRunning = true;
        NOTICE(
            "vmware-gfx entered mode "
            << Dec << actualWidth << "x" << actualHeight << "x" << actualDepth
            << Hex << ", mode framebuffer is " << (fbBase + fbOffset));
        return true;
    }

    bool submitFifoLocked(const uint32_t *pCommand, size_t words)
    {
        if (!availableLocked() || !m_FifoRunning || !pCommand ||
            (words > LargestCommandWords))
            return false;

        VmwareSvgaState::FifoLayout layout;
        if (!prepareFifoLocked(words, layout))
            return false;

        volatile uint32_t *fifo = fifoLocked();
        uint32_t next = layout.next;
        for (size_t i = 0; i < words; ++i)
        {
            fifo[next / sizeof(uint32_t)] = pCommand[i];
            next += sizeof(uint32_t);
            if (next == layout.max)
                next = layout.min;
        }

        // Publish NEXT_CMD only after every word of the command is visible.
        asm volatile("" : : : "memory");
        fifo[SVGA_FIFO_NEXT_CMD] = next;
        asm volatile("" : : : "memory");
        return true;
    }

    bool prepareFifoLocked(size_t words, VmwareSvgaState::FifoLayout &layout)
    {
        layout = readFifoLayoutLocked();
        if (!VmwareSvgaState::valid(
                layout, m_FifoHeaderBytes, m_CommandRegionSize))
        {
            markOfflineLocked("device returned an invalid FIFO cursor");
            return false;
        }
        if (VmwareSvgaState::canFit(layout, words))
            return true;

        if (!syncFifoLocked())
        {
            markOfflineLocked("timed out waiting for FIFO space");
            return false;
        }

        layout = readFifoLayoutLocked();
        if (!VmwareSvgaState::valid(
                layout, m_FifoHeaderBytes, m_CommandRegionSize) ||
            !VmwareSvgaState::canFit(layout, words))
        {
            markOfflineLocked("FIFO remained full after a completed sync");
            return false;
        }
        return true;
    }

    bool syncFifoLocked()
    {
        writeRegisterLocked(SVGA_REG_SYNC, 1);
        VmwareSvgaState::PollBudget budget(Time::getTicks());
        while (readRegisterLocked(SVGA_REG_BUSY))
        {
            if (!budget.keepPolling(Time::getTicks()))
                return false;
            Processor::pause();
        }
        return true;
    }

    VmwareSvgaState::FifoLayout readFifoLayoutLocked() const
    {
        volatile uint32_t *fifo = fifoLocked();
        return {
            fifo[SVGA_FIFO_MIN], fifo[SVGA_FIFO_MAX], fifo[SVGA_FIFO_NEXT_CMD],
            fifo[SVGA_FIFO_STOP]};
    }

    volatile uint32_t *fifoLocked() const
    {
        return reinterpret_cast<volatile uint32_t *>(
            m_CommandRegion->virtualAddress());
    }

    uint32_t readRegisterLocked(size_t offset) const
    {
        m_pIo->write32(static_cast<uint32_t>(offset), SVGA_INDEX_PORT);
        return m_pIo->read32(SVGA_VALUE_PORT);
    }

    void writeRegisterLocked(size_t offset, uint32_t value) const
    {
        m_pIo->write32(static_cast<uint32_t>(offset), SVGA_INDEX_PORT);
        m_pIo->write32(value, SVGA_VALUE_PORT);
    }

    void disableDisplayLocked()
    {
        if (!m_HardwareAccepted)
            return;
        writeRegisterLocked(SVGA_REG_CONFIG_DONE, 0);
        writeRegisterLocked(SVGA_REG_ENABLE, 0);
        m_FifoRunning = false;
        if (m_pFramebuffer)
            m_pFramebuffer->setActive(false);
    }

    void disableHardwareLocked()
    {
        disableDisplayLocked();
        m_Online = false;
    }

    void markOfflineLocked(const char *reason)
    {
        WARNING("vmware-gfx offline: " << reason);
        disableHardwareLocked();
    }

    IoBase *m_pIo;
    MemoryMappedIo *m_Framebuffer;
    MemoryMappedIo *m_CommandRegion;
    VmwareFramebuffer *m_pFramebuffer;
    GraphicsService::GraphicsProvider *m_pProvider;
    Mutex m_DeviceLock;
    uint32_t m_Capabilities;
    size_t m_MaxWidth;
    size_t m_MaxHeight;
    size_t m_FramebufferSize;
    size_t m_CommandRegionSize;
    size_t m_FifoHeaderBytes;
    bool m_HardwareAccepted;
    bool m_Online;
    bool m_FifoRunning;
    bool m_Stopping;
    bool m_ProviderRegistered;
    bool m_ShutdownComplete;
};

VmwareGraphics::~VmwareGraphics()
{
    shutdown();
}

void VmwareGraphics::shutdown()
{
    VmwareFramebuffer *pFramebuffer = nullptr;
    {
        LockGuard<Mutex> guard(m_DeviceLock);
        if (m_ShutdownComplete)
            return;
        if (m_Stopping)
            panic("vmware-gfx encountered concurrent teardown");
        m_Stopping = true;
        pFramebuffer = m_pFramebuffer;
    }

    // Reject new callbacks before the provider disappears from discovery.
    // Operations admitted before the close are drained before state is freed.
    if (pFramebuffer)
        pFramebuffer->closeCallbacks();
    unregisterProvider();
    if (pFramebuffer)
        pFramebuffer->detach();

    GraphicsService::GraphicsProvider *pProvider = nullptr;
    {
        LockGuard<Mutex> guard(m_DeviceLock);
        if (m_FifoRunning && !syncFifoLocked())
            WARNING("vmware-gfx timed out draining FIFO during teardown");
        disableHardwareLocked();
        pFramebuffer = m_pFramebuffer;
        pProvider = m_pProvider;
        m_pFramebuffer = nullptr;
        m_pProvider = nullptr;
        m_ShutdownComplete = true;
    }

    delete pFramebuffer;
    delete pProvider;
}

static bool g_Found = false;
static List<VmwareGraphics *> g_Displays;

static void probeDevice(Device *pDevice)
{
    Device *pParent = pDevice->getParent();
    if (!pParent)
        panic("vmware-gfx found a detached PCI device");
    if (pDevice->getNumChildren())
    {
        ERROR("vmware-gfx cannot bind a non-leaf PCI device");
        return;
    }

    VmwareGraphics *pGraphics = new VmwareGraphics(pDevice);
    if (!pGraphics)
    {
        ERROR("vmware-gfx could not allocate device state");
        return;
    }
    pGraphics->setParent(pParent);
    pParent->replaceChild(pDevice, pGraphics);
    // Device(Device *) recreated every BAR and invalidated the source mappings.
    // The leaf check makes deleting the now-inert source safe.
    delete pDevice;

    g_Displays.pushBack(pGraphics);
    g_Found = true;
    if (!pGraphics->initialise())
        ERROR("vmware-gfx device initialisation failed; device left offline");
}

static bool entry()
{
    g_Found = false;
    Device::searchByVendorIdAndDeviceId(
        PCI_VENDOR_ID_VMWARE, PCI_DEVICE_ID_VMWARE_SVGA2, probeDevice);
    return g_Found;
}

static void exit()
{
    auto restoreDisplay = [](Device *pDevice, Device *pTarget,
                             bool *pRestored) -> Device * {
        if (pDevice != pTarget)
            return pDevice;
        *pRestored = true;
        return new Device(pTarget);
    };
    auto callback = pedigree_std::make_callable(restoreDisplay);
    while (g_Displays.count())
    {
        VmwareGraphics *pDisplay = g_Displays.popFront();
        // Device(Device *) destroys the source I/O objects while recreating
        // the BARs, so no driver callback may still be admitted at that point.
        pDisplay->shutdown();
        bool restored = false;
        Device::foreach (callback, nullptr, pDisplay, &restored);
        if (!restored)
            delete pDisplay;
    }
    g_Found = false;
}

MODULE_INFO("vmware-gfx", &entry, &exit, "pci", "config");
