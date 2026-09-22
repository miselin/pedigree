#ifndef LIBUI_BUFFER_H
#define LIBUI_BUFFER_H

#include "pedigree/native/ipc/Ipc.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "libui/types.h"

namespace libui::buffer {

struct Size {
  std::int32_t width = 0;
  std::int32_t height = 0;
};

struct BufferHandle {
  std::vector<std::uint8_t> bytes;

  bool valid() const {
    return !bytes.empty();
  }
  bool operator==(const BufferHandle& other) const {
    return bytes == other.bytes;
  }
};

enum class State { Free, ClientDrawing, Submitted, Compositing, Released };

class SharedBuffer {
 public:
  SharedBuffer() = default;
  ~SharedBuffer();

  SharedBuffer(const SharedBuffer&) = delete;
  SharedBuffer& operator=(const SharedBuffer&) = delete;
  SharedBuffer(SharedBuffer&& other) noexcept;
  SharedBuffer& operator=(SharedBuffer&& other) noexcept;

  static SharedBuffer create(Size size, PixelFormat format = PixelFormat::CairoArgb32);
  static SharedBuffer open(const BufferHandle& handle, Size size, PixelFormat format,
                           std::size_t stride, std::size_t byteSize);

  bool valid() const {
    return m_message != nullptr && m_data != nullptr;
  }
  const BufferHandle& handle() const {
    return m_handle;
  }
  Size size() const {
    return m_size;
  }
  PixelFormat format() const {
    return m_format;
  }
  std::size_t stride() const {
    return m_stride;
  }
  std::size_t byteSize() const {
    return m_byteSize;
  }
  std::uint8_t* data() {
    return static_cast<std::uint8_t*>(m_data);
  }
  const std::uint8_t* data() const {
    return static_cast<const std::uint8_t*>(m_data);
  }

  // Pedigree shared messages are immutable in size. A resize is represented
  // by attaching new buffers in a new generation instead.
  bool resize(Size size);
  void destroy();
  void fill(std::uint32_t value);

 private:
  SharedBuffer(std::unique_ptr<PedigreeIpc::SharedIpcMessage> message, BufferHandle handle,
               Size size, PixelFormat format, std::size_t stride, std::size_t byteSize, void* data)
      : m_message(std::move(message)),
        m_handle(std::move(handle)),
        m_size(size),
        m_format(format),
        m_stride(stride),
        m_byteSize(byteSize),
        m_data(data) {}

  std::unique_ptr<PedigreeIpc::SharedIpcMessage> m_message;
  BufferHandle m_handle;
  Size m_size;
  PixelFormat m_format = PixelFormat::Unknown;
  std::size_t m_stride = 0;
  std::size_t m_byteSize = 0;
  void* m_data = nullptr;
};

class BufferPool {
 public:
  explicit BufferPool(Size size, std::size_t count = 2);

  SharedBuffer* acquire();
  bool submit(SharedBuffer* buffer);
  bool beginCompositing(SharedBuffer* buffer);
  bool release(SharedBuffer* buffer);
  void resize(Size size);
  Size size() const {
    return m_size;
  }
  std::uint64_t generation() const {
    return m_generation;
  }

 private:
  struct Entry {
    SharedBuffer buffer;
    State state = State::Free;
    std::uint64_t generation = 1;
  };

  Size m_size;
  std::size_t m_targetCount;
  std::uint64_t m_generation = 1;
  std::vector<std::unique_ptr<Entry>> m_entries;
};

}  // namespace libui::buffer

#endif
