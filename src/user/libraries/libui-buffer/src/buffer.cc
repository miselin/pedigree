#include "libui/buffer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace libui::buffer {
namespace {

bool validSize(Size size) {
  return size.width > 0 && size.height > 0;
}

bool byteSizeFor(Size size, std::size_t& bytes) {
  if (!validSize(size)) {
    return false;
  }

  const std::size_t width = static_cast<std::size_t>(size.width);
  const std::size_t height = static_cast<std::size_t>(size.height);
  if (width > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
    return false;
  }
  const std::size_t stride = width * sizeof(std::uint32_t);
  if (height > std::numeric_limits<std::size_t>::max() / stride) {
    return false;
  }
  bytes = stride * height;
  return true;
}

BufferHandle handleFor(void* handle) {
  BufferHandle result;
  result.bytes.resize(sizeof(handle));
  std::memcpy(result.bytes.data(), &handle, sizeof(handle));
  return result;
}

void* handleFrom(const BufferHandle& handle) {
  if (handle.bytes.size() != sizeof(void*)) {
    return nullptr;
  }

  void* result = nullptr;
  std::memcpy(&result, handle.bytes.data(), sizeof(result));
  return result;
}

}  // namespace

SharedBuffer::~SharedBuffer() {
  destroy();
}

SharedBuffer::SharedBuffer(SharedBuffer&& other) noexcept
    : m_message(std::move(other.m_message)),
      m_handle(std::move(other.m_handle)),
      m_size(other.m_size),
      m_format(other.m_format),
      m_stride(other.m_stride),
      m_byteSize(other.m_byteSize),
      m_data(other.m_data) {
  other.m_handle.bytes.clear();
  other.m_format = PixelFormat::Unknown;
  other.m_stride = 0;
  other.m_byteSize = 0;
  other.m_data = nullptr;
}

SharedBuffer& SharedBuffer::operator=(SharedBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroy();
  m_message = std::move(other.m_message);
  m_handle = std::move(other.m_handle);
  m_size = other.m_size;
  m_format = other.m_format;
  m_stride = other.m_stride;
  m_byteSize = other.m_byteSize;
  m_data = other.m_data;
  other.m_handle.bytes.clear();
  other.m_format = PixelFormat::Unknown;
  other.m_stride = 0;
  other.m_byteSize = 0;
  other.m_data = nullptr;
  return *this;
}

SharedBuffer SharedBuffer::create(Size size, PixelFormat format) {
  if (!validSize(size) || format != PixelFormat::CairoArgb32) {
    return {};
  }

  std::size_t bytes = 0;
  if (!byteSizeFor(size, bytes)) {
    return {};
  }

  auto message = std::make_unique<PedigreeIpc::SharedIpcMessage>(bytes, nullptr);
  if (!message->initialise()) {
    return {};
  }

  const std::size_t stride = static_cast<std::size_t>(size.width) * sizeof(std::uint32_t);
  const BufferHandle handle = handleFor(message->getHandle());
  void* data = message->getBuffer();
  return SharedBuffer(std::move(message), handle, size, format, stride, bytes, data);
}

SharedBuffer SharedBuffer::open(const BufferHandle& handle, Size size, PixelFormat format,
                                std::size_t stride, std::size_t byteSize) {
  if (!validSize(size) || !handle.valid() || format != PixelFormat::CairoArgb32) {
    return {};
  }

  std::size_t expectedByteSize = 0;
  if (!byteSizeFor(size, expectedByteSize) ||
      stride != static_cast<std::size_t>(size.width) * sizeof(std::uint32_t) ||
      byteSize != expectedByteSize) {
    return {};
  }

  void* existingHandle = handleFrom(handle);
  if (!existingHandle) {
    return {};
  }

  auto message = std::make_unique<PedigreeIpc::SharedIpcMessage>(byteSize, existingHandle);
  if (!message->initialise()) {
    return {};
  }

  void* data = message->getBuffer();
  return SharedBuffer(std::move(message), handle, size, format, stride, byteSize, data);
}

bool SharedBuffer::resize(Size) {
  return false;
}

void SharedBuffer::destroy() {
  m_message.reset();
  m_handle.bytes.clear();
  m_format = PixelFormat::Unknown;
  m_stride = 0;
  m_byteSize = 0;
  m_data = nullptr;
}

void SharedBuffer::fill(std::uint32_t value) {
  if (!valid()) {
    return;
  }

  auto* pixels = static_cast<std::uint32_t*>(m_data);
  const std::size_t rowPixels = m_stride / sizeof(std::uint32_t);
  for (std::int32_t y = 0; y < m_size.height; ++y) {
    std::fill(pixels + static_cast<std::size_t>(y) * rowPixels,
              pixels + static_cast<std::size_t>(y) * rowPixels + m_size.width, value);
  }
}

BufferPool::BufferPool(Size size, std::size_t count) : m_size(size), m_targetCount(count) {
  for (std::size_t index = 0; index < count; ++index) {
    auto entry = std::make_unique<Entry>();
    entry->buffer = SharedBuffer::create(size);
    if (entry->buffer.valid()) {
      m_entries.push_back(std::move(entry));
    }
  }
}

SharedBuffer* BufferPool::acquire() {
  for (auto& entry : m_entries) {
    if ((entry->state == State::Free || entry->state == State::Released) &&
        entry->generation == m_generation) {
      entry->state = State::ClientDrawing;
      return &entry->buffer;
    }
  }
  return nullptr;
}

bool BufferPool::submit(SharedBuffer* buffer) {
  for (auto& entry : m_entries) {
    if (&entry->buffer == buffer && entry->state == State::ClientDrawing) {
      entry->state = State::Submitted;
      return true;
    }
  }
  return false;
}

bool BufferPool::beginCompositing(SharedBuffer* buffer) {
  for (auto& entry : m_entries) {
    if (&entry->buffer == buffer && entry->state == State::Submitted) {
      entry->state = State::Compositing;
      return true;
    }
  }
  return false;
}

bool BufferPool::release(SharedBuffer* buffer) {
  for (auto& entry : m_entries) {
    if (&entry->buffer == buffer && entry->state == State::Compositing) {
      entry->state = State::Released;
      return true;
    }
  }
  return false;
}

void BufferPool::resize(Size size) {
  if (!validSize(size)) {
    return;
  }

  m_size = size;
  ++m_generation;
  for (auto& entry : m_entries) {
    if (entry->state == State::Free || entry->state == State::Released) {
      entry->buffer = SharedBuffer::create(size);
      if (entry->buffer.valid()) {
        entry->generation = m_generation;
        entry->state = State::Free;
      }
    }
  }

  std::size_t currentGenerationCount = 0;
  for (const auto& entry : m_entries) {
    if (entry->generation == m_generation) {
      ++currentGenerationCount;
    }
  }
  while (currentGenerationCount < m_targetCount) {
    auto entry = std::make_unique<Entry>();
    entry->buffer = SharedBuffer::create(size);
    if (!entry->buffer.valid()) {
      break;
    }
    entry->generation = m_generation;
    m_entries.push_back(std::move(entry));
    ++currentGenerationCount;
  }
}

}  // namespace libui::buffer
