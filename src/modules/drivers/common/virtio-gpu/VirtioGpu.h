/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_VIRTIO_GPU_H
#define PEDIGREE_VIRTIO_GPU_H

#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class Thread;

class VirtioGpu final : public Display {
 public:
  explicit VirtioGpu(Device* pci);
  ~VirtioGpu() override;

  bool initialise();
  void shutdown();
  void redraw(size_t x, size_t y, size_t width, size_t height);

  void getName(String& name) override;
  void dump(String& description) override;
  void* getFramebuffer() override;
  bool getPixelFormat(PixelFormat& format) override;
  bool getCurrentScreenMode(ScreenMode& mode) override;
  bool getScreenModes(List<ScreenMode*>& modes) override;
  bool setScreenMode(ScreenMode mode) override;
  bool setScreenMode(size_t modeId) override;
  bool setScreenMode(size_t width, size_t height, size_t bpp) override;

 private:
  class GpuFramebuffer final : public Framebuffer {
   public:
    explicit GpuFramebuffer(VirtioGpu* gpu);
    physical_uintptr_t getPhysicalPage(size_t offset) const override;
    void closeCallbacks();
    void detach();

   protected:
    void hwRedraw(size_t x, size_t y, size_t width, size_t height) override;

   private:
    OperationBarrier m_Callbacks;
    VirtioGpu* m_Gpu;
  };

  static int refreshThread(void* context);
  bool transact(const void* request, size_t requestLength, uint32_t expectedResponse,
                void* response, size_t responseLength);
  bool update(size_t x, size_t y, size_t width, size_t height);
  bool registerProvider();
  void unregisterProvider();

  Device* m_Pci;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_ControlQueue;
  MemoryRegion m_Commands;
  MemoryRegion m_Backing;
  MemoryRegion m_Pixels;
  Mutex m_CommandLock;
  GpuFramebuffer* m_Framebuffer;
  GraphicsService::GraphicsProvider* m_Provider;
  Thread* m_RefreshThread;
  ScreenMode m_Mode;
  uint64_t m_NextFence;
  uint32_t m_Scanout;
  bool m_Ready;
  bool m_Failed;
  bool m_Registered;
  bool m_Shutdown;
  bool m_Stopping;
};

#endif
