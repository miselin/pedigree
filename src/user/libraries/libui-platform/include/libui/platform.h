#ifndef LIBUI_PLATFORM_H
#define LIBUI_PLATFORM_H

#include <cairo.h>
#include <memory>
#include <string>

#include "libui/types.h"

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

struct DisplayDamage {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool empty() const {
    return width <= 0 || height <= 0;
  }
};

class Display {
 public:
  virtual ~Display() = default;

  virtual bool initialise() = 0;
  virtual bool open(const DisplayOptions& options) = 0;
  virtual const DisplayInfo& info() const = 0;
  virtual cairo_t* context() const = 0;
  // The backend may publish only this scene region; an empty region means
  // that the scene is unchanged and only transient overlays may move.
  virtual void present(const DisplayDamage& damage = {}) = 0;
  virtual bool poll(input::Event& event) = 0;
  virtual bool waitForInput(int timeoutMilliseconds) = 0;
  // The selected cursor is composited during the following present.
  virtual void renderCursor(input::CursorType type) = 0;
  virtual int pointerX() const = 0;
  virtual int pointerY() const = 0;
};

std::unique_ptr<Display> createDisplay();

}  // namespace libui::platform

#endif
