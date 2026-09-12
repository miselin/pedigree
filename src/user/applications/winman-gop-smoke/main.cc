#include "libui/platform.h"

#include <cairo.h>
#include <pedigree/log.h>

#include <algorithm>
#include <cstdio>
#include <memory>

namespace {

constexpr int TaskbarHeight = 28;
constexpr int FrameWidth = 420;
constexpr int FrameHeight = 260;

void paint(libui::platform::Display& display, int pointerX, int pointerY) {
  cairo_t* cr = display.context();
  const libui::platform::DisplayInfo& info = display.info();

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgb(cr, 0.0, 0.5, 0.5);
  cairo_paint(cr);

  const int taskbarY = info.height - TaskbarHeight;
  cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
  cairo_rectangle(cr, 0, taskbarY, info.width, TaskbarHeight);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr, 0, taskbarY, info.width, 2);
  cairo_fill(cr);

  const int frameX = std::max(8, (info.width - FrameWidth) / 2);
  const int frameY = std::max(8, (taskbarY - FrameHeight) / 2);
  cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
  cairo_rectangle(cr, frameX, frameY, FrameWidth, FrameHeight);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_rectangle(cr, frameX, frameY, FrameWidth, 2);
  cairo_rectangle(cr, frameX, frameY, 2, FrameHeight);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_rectangle(cr, frameX, frameY + FrameHeight - 2, FrameWidth, 2);
  cairo_rectangle(cr, frameX + FrameWidth - 2, frameY, 2, FrameHeight);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.5);
  cairo_rectangle(cr, frameX + 8, frameY + 8, FrameWidth - 16, 24);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_set_font_size(cr, 14);
  cairo_move_to(cr, frameX + 16, frameY + 25);
  cairo_show_text(cr, "Pedigree GOP display backend");

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, pointerX - 5, pointerY);
  cairo_line_to(cr, pointerX + 6, pointerY);
  cairo_move_to(cr, pointerX, pointerY - 5);
  cairo_line_to(cr, pointerX, pointerY + 6);
  cairo_stroke(cr);

  display.present();
}

}  // namespace

int main() {
  std::unique_ptr<libui::platform::Display> display = libui::platform::createDisplay();
  if (!display || !display->initialise()) {
    std::fprintf(stderr, "winman-gop-smoke: display initialisation failed\n");
    return 1;
  }

  libui::platform::DisplayOptions options;
  options.title = "Pedigree GOP display backend";
  options.clearColor = {0, 128, 128, 255};
  if (!display->open(options)) {
    std::fprintf(stderr, "winman-gop-smoke: display open failed\n");
    return 1;
  }

  const libui::platform::DisplayInfo& info = display->info();
  pedigree_log(LOG_INFO, "winman-gop-smoke: mode=%dx%d stride=%d format=%u", info.width,
               info.height, info.stride, static_cast<unsigned>(info.format));
  paint(*display, display->pointerX(), display->pointerY());

  while (true) {
    display->waitForInput(1000);

    libui::input::Event event;
    while (display->poll(event)) {
      if (event.type == libui::input::Event::Type::PointerMove ||
          event.type == libui::input::Event::Type::PointerDown ||
          event.type == libui::input::Event::Type::PointerUp) {
        paint(*display, event.x, event.y);
      }
      std::fprintf(stderr, "winman-gop-smoke: input type=%d x=%d y=%d button=%u\n",
                   static_cast<int>(event.type), event.x, event.y, event.button);
    }
  }
}
