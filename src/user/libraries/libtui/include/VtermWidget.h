#ifndef PEDIGREE_TUI_VTERM_WIDGET_H
#define PEDIGREE_TUI_VTERM_WIDGET_H

#include "pedigree/native/graphics/Graphics.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <vterm.h>

#include <cairo/cairo.h>

class DirtyRectangle;
class Font;
class Terminal;
class Tui;
class Widget;

class Vterm {
 public:
  Vterm(PedigreeGraphics::Framebuffer* framebuffer, size_t width, size_t height, size_t offsetLeft,
        size_t offsetTop, Terminal* terminal, Widget* widget, Tui* tui, Font* normalFont,
        Font* boldFont);
  ~Vterm();

  void write(const char* bytes, size_t length, DirtyRectangle& rect);
  void renderAll(DirtyRectangle& rect);
  void processKey(uint64_t key);
  void resize(size_t width, size_t height, PedigreeGraphics::Framebuffer* framebuffer);

  size_t getRows() const {
    return m_Rows;
  }
  size_t getCols() const {
    return m_Cols;
  }

  void showCursor(DirtyRectangle& rect);
  void hideCursor(DirtyRectangle& rect);
  void setCursorStyle(bool filled) {
    m_CursorFilled = filled;
  }
  void setCairo(cairo_t* cairo, cairo_surface_t* surface);
  void setFonts(Font* normalFont, Font* boldFont);

 private:
  Vterm(const Vterm&) = delete;
  Vterm& operator=(const Vterm&) = delete;

  static void outputCallback(const char* bytes, size_t length, void* user);
  static int damageCallback(VTermRect rect, void* user);
  static int moveRectCallback(VTermRect dest, VTermRect src, void* user);
  static int moveCursorCallback(VTermPos pos, VTermPos oldPos, int visible, void* user);
  static int setTermPropCallback(VTermProp prop, VTermValue* value, void* user);
  static int bellCallback(void* user);
  static int resizeCallback(int rows, int cols, void* user);
  static int scrollbackPushCallback(int cols, const VTermScreenCell* cells, void* user);
  static int scrollbackPopCallback(int cols, VTermScreenCell* cells, void* user);
  static int scrollbackClearCallback(void* user);

  void syncRect(VTermRect rect);
  void queueDamage(VTermRect rect);
  void renderPending(DirtyRectangle& rect);
  void renderCell(DirtyRectangle& rect, int row, int col, bool cursor);
  void markCell(DirtyRectangle& rect, VTermPos pos);
  bool blitMovedRect(VTermRect dest, VTermRect src);
  uint32_t color(const VTermColor& source, bool foreground) const;
  void sendInput(const char* bytes, size_t length);

  VTerm* m_Vterm;
  VTermScreen* m_Screen;
  VTermState* m_State;
  std::vector<VTermScreenCell> m_Cells;
  std::vector<VTermRect> m_PendingDamage;
  std::vector<std::vector<VTermScreenCell>> m_Scrollback;

  PedigreeGraphics::Framebuffer* m_Framebuffer;
  cairo_t* m_Cairo;
  cairo_surface_t* m_CairoSurface;
  Terminal* m_Terminal;
  Widget* m_Widget;
  Tui* m_Tui;
  Font* m_NormalFont;
  Font* m_BoldFont;

  size_t m_Rows;
  size_t m_Cols;
  size_t m_OffsetLeft;
  size_t m_OffsetTop;
  VTermPos m_Cursor;
  bool m_CursorVisible;
  bool m_CursorFilled;
  bool m_Rendering;
  DirtyRectangle* m_ActiveRect;
};

#endif
