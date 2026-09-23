/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "VirtioGpu.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr uint32_t Create2d = 0x0101;
constexpr uint32_t SetScanout = 0x0103;
constexpr uint32_t Flush = 0x0104;
constexpr uint32_t Transfer2d = 0x0105;
constexpr uint32_t AttachBacking = 0x0106;
constexpr uint32_t GetDisplayInfo = 0x0100;
constexpr uint32_t OkNoData = 0x1100;
constexpr uint32_t OkDisplayInfo = 0x1101;
constexpr uint32_t Fence = 1;
constexpr uint32_t BgrxFormat = 2;
constexpr uint32_t ResourceId = 1;
constexpr size_t MaxWidth = 1920;
constexpr size_t MaxHeight = 1200;
constexpr size_t RefreshMilliseconds = 50;
constexpr size_t ResponseOffset = 512;

struct Header {
  uint32_t type;
  uint32_t flags;
  uint64_t fenceId;
  uint32_t contextId;
  uint8_t ringIndex;
  uint8_t padding[3];
} __attribute__((packed));

struct Rect {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

struct DisplayInfo {
  Header header;
  struct {
    Rect rect;
    uint32_t enabled;
    uint32_t flags;
  } scanouts[16];
} __attribute__((packed));

struct CreateResource {
  Header header;
  uint32_t resourceId;
  uint32_t format;
  uint32_t width;
  uint32_t height;
} __attribute__((packed));

struct ResourceRectangle {
  Header header;
  Rect rect;
  uint32_t resourceId;
  uint32_t extra;
} __attribute__((packed));

struct TransferResource {
  Header header;
  Rect rect;
  uint64_t offset;
  uint32_t resourceId;
  uint32_t padding;
} __attribute__((packed));

struct BackingEntry {
  uint64_t address;
  uint32_t length;
  uint32_t padding;
} __attribute__((packed));

struct AttachResource {
  Header header;
  uint32_t resourceId;
  uint32_t entries;
} __attribute__((packed));

static_assert(sizeof(Header) == 24, "virtio GPU control header has the wrong layout");
static_assert(sizeof(DisplayInfo) == 408, "virtio GPU display response has the wrong layout");

size_t pagesFor(size_t bytes) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  return (bytes + pageSize - 1) / pageSize;
}
}  // namespace

VirtioGpu::GpuFramebuffer::GpuFramebuffer(VirtioGpu* gpu)
    : Framebuffer(), m_Callbacks(), m_Gpu(gpu) {}

physical_uintptr_t VirtioGpu::GpuFramebuffer::getPhysicalPage(size_t offset) const {
  if (!m_Gpu || offset >= getHeight() * getBytesPerLine()) {
    return ~physical_uintptr_t(0);
  }
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  auto* address =
      static_cast<uint8_t*>(m_Gpu->m_Pixels.virtualAddress()) + (offset & ~(pageSize - 1));
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!VirtualAddressSpace::getKernelAddressSpace().getMapping(address, physical, flags)) {
    return ~physical_uintptr_t(0);
  }
  return physical;
}

void VirtioGpu::GpuFramebuffer::closeCallbacks() {
  m_Callbacks.close();
}

void VirtioGpu::GpuFramebuffer::detach() {
  m_Callbacks.wait();
  m_Gpu = nullptr;
  setActive(false);
  setFramebuffer(0);
}

void VirtioGpu::GpuFramebuffer::hwRedraw(size_t x, size_t y, size_t width, size_t height) {
  OperationBarrier::Lease callback;
  if (!m_Callbacks.tryAcquire(callback) || !m_Gpu) {
    return;
  }
  m_Gpu->redraw(x, y, width, height);
}

VirtioGpu::VirtioGpu(Device* pci)
    : Display(),
      m_Pci(pci),
      m_Transport(pci),
      m_ControlQueue(),
      m_Commands("virtio-gpu commands"),
      m_Backing("virtio-gpu backing list"),
      m_Pixels("virtio-gpu framebuffer"),
      m_CommandLock(),
      m_Framebuffer(nullptr),
      m_Provider(nullptr),
      m_RefreshThread(nullptr),
      m_Mode(),
      m_NextFence(0),
      m_Scanout(0),
      m_Ready(false),
      m_Failed(false),
      m_Registered(false),
      m_Shutdown(false),
      m_Stopping(false) {
  setSpecificType(String("virtio-gpu-display"));
}

VirtioGpu::~VirtioGpu() {
  shutdown();
}

bool VirtioGpu::transact(const void* request, size_t requestLength, uint32_t expectedResponse,
                         void* response, size_t responseLength) {
  LockGuard<Mutex> guard(m_CommandLock);
  if (!m_Ready || __atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&m_Stopping, __ATOMIC_ACQUIRE) || !request ||
      requestLength < sizeof(Header) || responseLength < sizeof(Header) ||
      responseLength > m_Commands.size() - ResponseOffset) {
    return false;
  }

  uint64_t requestPhysical = 0;
  uint8_t* requestBytes = nullptr;
  if (requestLength <= ResponseOffset) {
    requestBytes = static_cast<uint8_t*>(m_Commands.virtualAddress());
    MemoryCopy(requestBytes, request, requestLength);
    requestPhysical = m_Commands.physicalAddress();
  } else if (request == m_Backing.virtualAddress() && requestLength <= m_Backing.size()) {
    requestBytes = static_cast<uint8_t*>(m_Backing.virtualAddress());
    requestPhysical = m_Backing.physicalAddress();
  } else {
    return false;
  }

  auto* header = reinterpret_cast<Header*>(requestBytes);
  header->flags = Fence;
  header->fenceId = ++m_NextFence;
  auto* responseBytes = static_cast<uint8_t*>(m_Commands.virtualAddress()) + ResponseOffset;
  ByteSet(responseBytes, 0, responseLength);

  Virtio::Buffer buffers[] = {
      {requestPhysical, static_cast<uint32_t>(requestLength), false},
      {m_Commands.physicalAddress() + ResponseOffset, static_cast<uint32_t>(responseLength), true},
  };
  if (!m_ControlQueue.submit(buffers, 2, this)) {
    __atomic_store_n(&m_Failed, true, __ATOMIC_RELEASE);
    ERROR("virtio-gpu: control queue submission failed");
    return false;
  }
  m_Transport.notify(0);

  const uint64_t deadline = Time::getTicks() + 500 * Time::Multiplier::Millisecond;
  Virtio::Completion completion{};
  while (Time::getTicks() < deadline) {
    if (m_ControlQueue.pop(completion)) {
      break;
    }
    Time::delay(Time::Multiplier::Millisecond);
  }
  if (completion.cookie != this || completion.length < responseLength ||
      completion.length > responseLength) {
    __atomic_store_n(&m_Failed, true, __ATOMIC_RELEASE);
    ERROR("virtio-gpu: control command timed out or returned an invalid length");
    return false;
  }

  FENCE();
  auto* result = reinterpret_cast<const Header*>(responseBytes);
  if (result->type != expectedResponse || !(result->flags & Fence) ||
      result->fenceId != header->fenceId) {
    __atomic_store_n(&m_Failed, true, __ATOMIC_RELEASE);
    ERROR("virtio-gpu: control command returned error " << Hex << result->type);
    return false;
  }
  if (response) {
    MemoryCopy(response, responseBytes, responseLength);
  }
  return true;
}

bool VirtioGpu::initialise() {
  if (!m_Pci || m_Pci->getPciVendorId() != 0x1af4 || m_Pci->getPciDeviceId() != 0x1050 ||
      !m_Transport.initialise() || !m_Transport.negotiate(0) ||
      !m_Transport.setupQueue(0, m_ControlQueue)) {
    return false;
  }

  auto& memory = PhysicalMemoryManager::instance();
  const size_t mapFlags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Commands, 1, PhysicalMemoryManager::continuous, mapFlags) ||
      !m_Transport.ready() || !PciBus::instance().updateCommand(m_Pci, 0, 0x400U)) {
    return false;
  }
  m_Ready = true;
  (void)m_Transport.readIsr();

  uint32_t scanoutCount = 0;
  if (!m_Transport.readDeviceConfig32(8, scanoutCount) || !scanoutCount || scanoutCount > 16) {
    return false;
  }
  const Header query = {GetDisplayInfo, 0, 0, 0, 0, {0, 0, 0}};
  DisplayInfo info{};
  if (!transact(&query, sizeof(query), OkDisplayInfo, &info, sizeof(info))) {
    return false;
  }

  size_t width = 1024;
  size_t height = 768;
  for (uint32_t i = 0; i < scanoutCount; ++i) {
    const auto& candidate = info.scanouts[i];
    if (candidate.enabled && candidate.rect.width && candidate.rect.height &&
        candidate.rect.width <= MaxWidth && candidate.rect.height <= MaxHeight) {
      m_Scanout = i;
      width = candidate.rect.width;
      height = candidate.rect.height;
      break;
    }
  }

  const size_t bytes = width * height * 4;
  const size_t pixelPages = pagesFor(bytes);
  const size_t backingBytes = sizeof(AttachResource) + pixelPages * sizeof(BackingEntry);
  if (!memory.allocateRegion(m_Pixels, pixelPages, 0, mapFlags) ||
      !memory.allocateRegion(m_Backing, pagesFor(backingBytes), PhysicalMemoryManager::continuous,
                             mapFlags)) {
    ERROR("virtio-gpu: framebuffer allocation failed");
    return false;
  }
  ByteSet(m_Pixels.virtualAddress(), 0, bytes);

  CreateResource create{};
  create.header.type = Create2d;
  create.resourceId = ResourceId;
  create.format = BgrxFormat;
  create.width = width;
  create.height = height;
  if (!transact(&create, sizeof(create), OkNoData, nullptr, sizeof(Header))) {
    return false;
  }

  auto* attach = static_cast<AttachResource*>(m_Backing.virtualAddress());
  ByteSet(attach, 0, backingBytes);
  attach->header.type = AttachBacking;
  attach->resourceId = ResourceId;
  attach->entries = pixelPages;
  auto* entries = reinterpret_cast<BackingEntry*>(
      static_cast<uint8_t*>(m_Backing.virtualAddress()) + sizeof(AttachResource));
  for (size_t i = 0; i < pixelPages; ++i) {
    auto* page =
        static_cast<uint8_t*>(m_Pixels.virtualAddress()) + i * PhysicalMemoryManager::getPageSize();
    physical_uintptr_t physical = 0;
    size_t flags = 0;
    if (!VirtualAddressSpace::getKernelAddressSpace().getMapping(page, physical, flags)) {
      return false;
    }
    entries[i].address = physical;
    entries[i].length = bytes - i * PhysicalMemoryManager::getPageSize();
    if (entries[i].length > PhysicalMemoryManager::getPageSize()) {
      entries[i].length = PhysicalMemoryManager::getPageSize();
    }
  }
  if (!transact(attach, backingBytes, OkNoData, nullptr, sizeof(Header))) {
    return false;
  }

  m_Mode.width = width;
  m_Mode.height = height;
  ResourceRectangle scanout{};
  scanout.header.type = SetScanout;
  scanout.rect.width = width;
  scanout.rect.height = height;
  scanout.resourceId = m_Scanout;
  scanout.extra = ResourceId;
  if (!transact(&scanout, sizeof(scanout), OkNoData, nullptr, sizeof(Header)) ||
      !update(0, 0, width, height)) {
    return false;
  }

  m_Framebuffer = new GpuFramebuffer(this);
  m_Provider = new GraphicsService::GraphicsProvider;
  if (!m_Framebuffer || !m_Provider) {
    ERROR("virtio-gpu: could not allocate graphics provider");
    return false;
  }
  m_Framebuffer->setWidth(width);
  m_Framebuffer->setHeight(height);
  m_Framebuffer->setBytesPerPixel(4);
  m_Framebuffer->setBytesPerLine(width * 4);
  m_Framebuffer->setFormat(Graphics::Bits32_Rgb);
  m_Framebuffer->setFramebuffer(reinterpret_cast<uintptr_t>(m_Pixels.virtualAddress()));

  m_Mode.id = 1;
  m_Mode.framebuffer = 0;
  m_Mode.pf.mRed = 8;
  m_Mode.pf.pRed = 16;
  m_Mode.pf.mGreen = 8;
  m_Mode.pf.pGreen = 8;
  m_Mode.pf.mBlue = 8;
  m_Mode.pf.pBlue = 0;
  m_Mode.pf.mAlpha = 0;
  m_Mode.pf.pAlpha = 24;
  m_Mode.pf.nBpp = 32;
  m_Mode.pf.nPitch = width * 4;
  m_Mode.pf2 = Graphics::Bits32_Rgb;
  m_Mode.bytesPerLine = width * 4;
  m_Mode.bytesPerPixel = 4;
  m_Mode.textMode = false;

  m_Provider->pDisplay = this;
  m_Provider->pFramebuffer = m_Framebuffer;
  m_Provider->maxWidth = width;
  m_Provider->maxHeight = height;
  m_Provider->maxDepth = 32;
  m_Provider->maxTextWidth = 0;
  m_Provider->maxTextHeight = 0;
  m_Provider->bHardwareAccel = false;
  m_Provider->bTextModes = false;
  m_Provider->bFirmwareFallback = false;
  if (!registerProvider()) {
    return false;
  }

  m_RefreshThread = new Thread(Scheduler::instance().getKernelProcess(), refreshThread, this,
                               nullptr, false, false, true);
  if (!m_RefreshThread) {
    ERROR("virtio-gpu: could not allocate refresh worker");
    return false;
  }
  m_RefreshThread->setName(String("virtio-gpu refresh"));
  if (!m_RefreshThread->start()) {
    ERROR("virtio-gpu: could not start refresh worker");
    __atomic_store_n(&m_Stopping, true, __ATOMIC_RELEASE);
    if (!m_RefreshThread->joinForCompletion()) {
      panic("virtio-gpu: failed refresh worker could not be joined");
    }
    m_RefreshThread = nullptr;
    return false;
  }
  NOTICE("virtio-gpu: scanout " << Dec << m_Scanout << " at " << width << "x" << height << "x32"
                                << Hex);
  return true;
}

bool VirtioGpu::update(size_t x, size_t y, size_t width, size_t height) {
  if (!width || !height) {
    return true;
  }
  TransferResource transfer{};
  transfer.header.type = Transfer2d;
  transfer.rect = {static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(width),
                   static_cast<uint32_t>(height)};
  transfer.offset = (y * m_Mode.width + x) * 4;
  transfer.resourceId = ResourceId;
  if (!transact(&transfer, sizeof(transfer), OkNoData, nullptr, sizeof(Header))) {
    return false;
  }
  ResourceRectangle flush{};
  flush.header.type = Flush;
  flush.rect = transfer.rect;
  flush.resourceId = ResourceId;
  return transact(&flush, sizeof(flush), OkNoData, nullptr, sizeof(Header));
}

void VirtioGpu::redraw(size_t x, size_t y, size_t width, size_t height) {
  if (!m_Ready || __atomic_load_n(&m_Stopping, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE) || x >= m_Mode.width || y >= m_Mode.height) {
    return;
  }
  if (width > m_Mode.width - x) {
    width = m_Mode.width - x;
  }
  if (height > m_Mode.height - y) {
    height = m_Mode.height - y;
  }
  (void)update(x, y, width, height);
}

int VirtioGpu::refreshThread(void* context) {
  auto* gpu = static_cast<VirtioGpu*>(context);
  while (!__atomic_load_n(&gpu->m_Stopping, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&gpu->m_Failed, __ATOMIC_ACQUIRE)) {
    Time::delay(RefreshMilliseconds * Time::Multiplier::Millisecond);
    if (!__atomic_load_n(&gpu->m_Stopping, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&gpu->m_Failed, __ATOMIC_ACQUIRE)) {
      gpu->redraw(0, 0, gpu->m_Mode.width, gpu->m_Mode.height);
    }
  }
  return 0;
}

bool VirtioGpu::registerProvider() {
  ServiceFeatures* features = ServiceManager::instance().enumerateOperations(String("graphics"));
  Service* service = ServiceManager::instance().getService(String("graphics"));
  if (!features || !service || !features->provides(ServiceFeatures::touch)) {
    ERROR("virtio-gpu: graphics service unavailable");
    return false;
  }
  if (!service->serve(ServiceFeatures::touch, m_Provider, sizeof(*m_Provider))) {
    ERROR("virtio-gpu: graphics service rejected provider");
    return false;
  }
  m_Registered = true;
  return true;
}

void VirtioGpu::unregisterProvider() {
  if (!m_Registered) {
    return;
  }
  Service* service = ServiceManager::instance().getService(String("graphics"));
  if (!service || !service->serve(ServiceFeatures::withdraw, m_Provider, sizeof(*m_Provider))) {
    panic("virtio-gpu: graphics provider could not be withdrawn");
  }
  m_Registered = false;
}

void VirtioGpu::shutdown() {
  if (m_Shutdown) {
    return;
  }
  __atomic_store_n(&m_Stopping, true, __ATOMIC_RELEASE);
  if (m_Framebuffer) {
    m_Framebuffer->closeCallbacks();
  }
  unregisterProvider();
  if (m_RefreshThread) {
    if (!m_RefreshThread->joinForCompletion()) {
      panic("virtio-gpu: refresh worker could not be joined");
    }
    m_RefreshThread = nullptr;
  }
  if (m_Framebuffer) {
    m_Framebuffer->detach();
  }
  if (!m_Transport.reset()) {
    panic("virtio-gpu: device did not stop DMA");
  }
  m_ControlQueue.stop();
  m_Ready = false;
  delete m_Provider;
  delete m_Framebuffer;
  m_Provider = nullptr;
  m_Framebuffer = nullptr;
  m_Shutdown = true;
}

void VirtioGpu::getName(String& name) {
  name.assign("virtio-gpu", 10);
}

void VirtioGpu::dump(String& description) {
  description.assign("virtio 2D graphics card", 23);
}

void* VirtioGpu::getFramebuffer() {
  return m_Pixels.virtualAddress();
}

bool VirtioGpu::getPixelFormat(PixelFormat& format) {
  format = m_Mode.pf;
  return m_Ready && !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE);
}

bool VirtioGpu::getCurrentScreenMode(ScreenMode& mode) {
  mode = m_Mode;
  return m_Ready && !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE);
}

bool VirtioGpu::getScreenModes(List<ScreenMode*>& modes) {
  if (!m_Ready || __atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE)) {
    return false;
  }
  auto* mode = new ScreenMode(m_Mode);
  if (!mode) {
    return false;
  }
  modes.pushBack(mode);
  return true;
}

bool VirtioGpu::setScreenMode(ScreenMode mode) {
  return m_Ready && !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE) && mode.id == m_Mode.id &&
         mode.width == m_Mode.width && mode.height == m_Mode.height && mode.pf.nBpp == 32;
}

bool VirtioGpu::setScreenMode(size_t modeId) {
  return m_Ready && !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE) &&
         (modeId == 0 || modeId == m_Mode.id);
}

bool VirtioGpu::setScreenMode(size_t width, size_t height, size_t bpp) {
  return m_Ready && !__atomic_load_n(&m_Failed, __ATOMIC_ACQUIRE) && width == m_Mode.width &&
         height == m_Mode.height && bpp == 32;
}
