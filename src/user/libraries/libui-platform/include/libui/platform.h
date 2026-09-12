#ifndef LIBUI_PLATFORM_H
#define LIBUI_PLATFORM_H

#include "libui/types.h"

#include <cairo.h>

#include <memory>
#include <string>

namespace libui::platform {

struct DisplayOptions {
  std::string title;
  Color clearColor;
};

struct DisplayInfo {
  int width = 0;
  int height = 0;
  int stride = 0;
  PixelFormat format = PixelFormat::Unknown;
};

class Display {
 public:
  virtual ~Display() = default;

  virtual bool initialise() = 0;
  virtual bool open(const DisplayOptions& options) = 0;
  virtual const DisplayInfo& info() const = 0;
  virtual cairo_t* context() const = 0;
  virtual void present() = 0;
  virtual bool poll(input::Event& event) = 0;
  virtual bool waitForInput(int timeoutMilliseconds) = 0;
  virtual void renderCursor(input::CursorType type) = 0;
  virtual int pointerX() const = 0;
  virtual int pointerY() const = 0;
};

std::unique_ptr<Display> createDisplay();

}  // namespace libui::platform

#endif
