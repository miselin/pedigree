#include "libui/platform.h"
#include "pedigree/native/input/Input.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <unistd.h>
#include <utility>

#include "pedigree_fb.h"

namespace libui::platform {
namespace {

class PedigreeDisplay;
PedigreeDisplay* g_display = nullptr;
std::mutex g_displayLock;

void inputCallback(Input::InputNotification& notification);

class PedigreeDisplay final : public Display {
 public:
  ~PedigreeDisplay() override {
    if (m_callbackInstalled) {
      Input::removeCallback(inputCallback);
      m_callbackInstalled = false;
    }
    {
      std::lock_guard<std::mutex> guard(g_displayLock);
      g_display = nullptr;
    }

    if (m_context) {
      cairo_destroy(m_context);
    }
    if (m_surface) {
      cairo_surface_destroy(m_surface);
    }
    if (m_inputPipe[0] >= 0) {
      close(m_inputPipe[0]);
    }
    if (m_inputPipe[1] >= 0) {
      close(m_inputPipe[1]);
    }
    if (m_inputStream >= 0) {
      close(m_inputStream);
    }
  }

  bool initialise() override {
    if (!m_framebuffer.initialise() || m_framebuffer.useCurrentMode() != 0) {
      return false;
    }

    m_inputStream = Input::openEventStream();
    if (m_inputStream < 0) {
      if (pipe(m_inputPipe) != 0) {
        return false;
      }
      setNonBlocking(m_inputPipe[0]);
      setNonBlocking(m_inputPipe[1]);
      setCloseOnExec(m_inputPipe[0]);
      setCloseOnExec(m_inputPipe[1]);
    } else {
      setCloseOnExec(m_inputStream);
    }
    return true;
  }

  bool open(const DisplayOptions& options) override {
    m_info.width = static_cast<int>(m_framebuffer.getWidth());
    m_info.height = static_cast<int>(m_framebuffer.getHeight());
    m_info.stride = static_cast<int>(m_framebuffer.getBytesPerLine());
    m_info.format = m_framebuffer.getFormat() == CAIRO_FORMAT_ARGB32 ? PixelFormat::CairoArgb32
                                                                     : PixelFormat::Unknown;

    if (m_info.width <= 0 || m_info.height <= 0 || m_info.stride <= 0 ||
        m_info.format == PixelFormat::Unknown) {
      return false;
    }

    m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m_info.width, m_info.height);
    if (cairo_surface_status(m_surface) != CAIRO_STATUS_SUCCESS) {
      return false;
    }
    m_offscreenStride = cairo_image_surface_get_stride(m_surface);

    m_context = cairo_create(m_surface);
    if (cairo_status(m_context) != CAIRO_STATUS_SUCCESS) {
      return false;
    }

    m_pointerX = m_info.width / 2;
    m_pointerY = m_info.height / 2;

    if (m_inputStream < 0) {
      {
        std::lock_guard<std::mutex> guard(g_displayLock);
        g_display = this;
      }
      Input::installCallback(Input::RawKey | Input::Mouse, inputCallback);
      m_callbackInstalled = true;
    }

    cairo_set_operator(m_context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(m_context, options.clearColor.r / 255.0, options.clearColor.g / 255.0,
                          options.clearColor.b / 255.0, options.clearColor.a / 255.0);
    cairo_paint(m_context);
    cairo_set_operator(m_context, CAIRO_OPERATOR_OVER);
    present();
    return true;
  }

  const DisplayInfo& info() const override {
    return m_info;
  }
  cairo_t* context() const override {
    return m_context;
  }

  void present(const DisplayDamage& damage = {}) override {
    (void)damage;
    if (!m_surface || !m_framebuffer.getFramebuffer()) {
      return;
    }

    cairo_surface_flush(m_surface);
    const auto* source = cairo_image_surface_get_data(m_surface);
    auto* target = static_cast<std::uint8_t*>(m_framebuffer.getFramebuffer());
    const std::size_t rowBytes = static_cast<std::size_t>(m_info.width) * 4;
    for (int y = 0; y < m_info.height; ++y) {
      std::memcpy(target + static_cast<std::size_t>(y) * m_info.stride,
                  source + static_cast<std::size_t>(y) * m_offscreenStride, rowBytes);
    }
    m_framebuffer.flush(0, 0, m_framebuffer.getWidth(), m_framebuffer.getHeight());
  }

  bool poll(input::Event& event) override {
    if (popEvent(event)) {
      return true;
    }

    if (m_inputStream >= 0) {
      while (true) {
        Input::InputNotification notification;
        if (Input::readEvent(m_inputStream, notification) !=
            static_cast<ssize_t>(sizeof(notification))) {
          return false;
        }
        enqueue(notification, false);
        if (popEvent(event)) {
          return true;
        }
      }
    }

    // Input callbacks may be delivered around syscalls, so pipe I/O must not
    // happen while the event state is locked.
    drainInputPipe();
    if (!popEvent(event)) {
      return false;
    }
    consumeInputMarker();
    return true;
  }

  bool waitForInput(int timeoutMilliseconds) override {
    {
      std::lock_guard<std::mutex> guard(m_inputLock);
      if (!m_events.empty()) {
        return true;
      }
    }

    const int descriptorFd = m_inputStream >= 0 ? m_inputStream : m_inputPipe[0];
    pollfd descriptor = {descriptorFd, POLLIN, 0};
    return ::poll(&descriptor, 1, timeoutMilliseconds) > 0;
  }

  void renderCursor(input::CursorType) override {}
  int pointerX() const override {
    std::lock_guard<std::mutex> guard(m_inputLock);
    return m_pointerX;
  }
  int pointerY() const override {
    std::lock_guard<std::mutex> guard(m_inputLock);
    return m_pointerY;
  }

  void enqueue(const Input::InputNotification& notification, bool signalWakeup) {
    std::size_t wakeups = 0;
    if (notification.type & Input::Mouse) {
      wakeups += enqueueMouse(notification);
    }
    if (notification.type & Input::RawKey) {
      input::Event event;
      event.type =
          notification.data.rawkey.keyUp ? input::Event::Type::KeyUp : input::Event::Type::KeyDown;
      event.scancode = notification.data.rawkey.scancode;
      {
        std::lock_guard<std::mutex> guard(m_inputLock);
        m_events.push_back(std::move(event));
      }
      ++wakeups;
    }

    if (signalWakeup) {
      for (std::size_t i = 0; i < wakeups; ++i) {
        signalInput();
      }
    }
  }

  bool popEvent(input::Event& event) {
    std::lock_guard<std::mutex> guard(m_inputLock);
    if (m_events.empty()) {
      return false;
    }

    event = std::move(m_events.front());
    m_events.pop_front();
    return true;
  }

 private:
  static void setNonBlocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
  }

  static void setCloseOnExec(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFD, 0);
    if (flags >= 0) {
      fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
  }

  std::size_t enqueueMouse(const Input::InputNotification& notification) {
    std::size_t eventCount = 0;
    std::lock_guard<std::mutex> guard(m_inputLock);

    const int oldX = m_pointerX;
    const int oldY = m_pointerY;
    m_pointerX = std::clamp(m_pointerX + static_cast<int>(notification.data.pointy.relx), 0,
                            m_info.width - 1);
    // PS/2 reports positive Y as movement toward the top of the screen.
    m_pointerY = std::clamp(m_pointerY - static_cast<int>(notification.data.pointy.rely), 0,
                            m_info.height - 1);

    std::uint32_t buttons = 0;
    for (std::uint32_t button = 0; button < 3; ++button) {
      if (notification.data.pointy.buttons[button]) {
        buttons |= 1u << button;
      }
    }

    if (oldX != m_pointerX || oldY != m_pointerY) {
      input::Event event;
      event.type = input::Event::Type::PointerMove;
      event.x = m_pointerX;
      event.y = m_pointerY;
      event.buttonBitmap = buttons;
      m_events.push_back(std::move(event));
      ++eventCount;
    }

    for (std::uint32_t button = 0; button < 3; ++button) {
      const std::uint32_t mask = 1u << button;
      if ((buttons & mask) == (m_buttonBitmap & mask)) {
        continue;
      }

      input::Event event;
      event.type =
          (buttons & mask) ? input::Event::Type::PointerDown : input::Event::Type::PointerUp;
      event.x = m_pointerX;
      event.y = m_pointerY;
      event.button = button + 1;
      event.buttonBitmap = buttons;
      m_events.push_back(std::move(event));
      ++eventCount;
    }
    m_buttonBitmap = buttons;
    return eventCount;
  }

  void signalInput() {
    const unsigned char marker = 1;
    const ssize_t result = write(m_inputPipe[1], &marker, sizeof(marker));
    (void)result;
  }

  void drainInputPipe() {
    unsigned char markers[32];
    while (read(m_inputPipe[0], markers, sizeof(markers)) > 0) {
    }
  }

  void consumeInputMarker() {
    unsigned char marker;
    read(m_inputPipe[0], &marker, sizeof(marker));
  }

  Framebuffer m_framebuffer;
  DisplayInfo m_info;
  cairo_surface_t* m_surface = nullptr;
  cairo_t* m_context = nullptr;
  int m_offscreenStride = 0;
  int m_inputStream = -1;
  int m_inputPipe[2] = {-1, -1};
  mutable std::mutex m_inputLock;
  std::deque<input::Event> m_events;
  int m_pointerX = 0;
  int m_pointerY = 0;
  std::uint32_t m_buttonBitmap = 0;
  bool m_callbackInstalled = false;
};

void inputCallback(Input::InputNotification& notification) {
  std::lock_guard<std::mutex> displayGuard(g_displayLock);
  if (g_display) {
    g_display->enqueue(notification, true);
  }
}

}  // namespace

std::unique_ptr<Display> createDisplay() {
  return std::make_unique<PedigreeDisplay>();
}

}  // namespace libui::platform
