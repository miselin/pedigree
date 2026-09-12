#include "VtermWidget.h"

#include <Font.h>
#include <Terminal.h>
#include <Widget.h>
#include <algorithm>
#include <cstring>
#include <tui.h>

#include "environment.h"

namespace {
constexpr uint8_t RenderBold = 1 << 0;
constexpr uint8_t RenderUnderline = 1 << 1;
constexpr uint8_t RenderItalic = 1 << 2;
constexpr uint8_t RenderReverse = 1 << 3;
constexpr uint8_t RenderStrike = 1 << 4;
constexpr size_t MaxScrollback = 2000;

bool intersectsOrTouches(const VTermRect& a, const VTermRect& b) {
  return a.start_row <= b.end_row && b.start_row <= a.end_row && a.start_col <= b.end_col &&
         b.start_col <= a.end_col;
}
}  // namespace

Vterm::Vterm(PedigreeGraphics::Framebuffer* framebuffer, size_t width, size_t height,
             size_t offsetLeft, size_t offsetTop, Terminal* terminal, Widget* widget, Tui* tui,
             Font* normalFont, Font* boldFont)
    : m_Vterm(nullptr),
      m_Screen(nullptr),
      m_State(nullptr),
      m_Framebuffer(framebuffer),
      m_Cairo(nullptr),
      m_CairoSurface(nullptr),
      m_Terminal(terminal),
      m_Widget(widget),
      m_Tui(tui),
      m_NormalFont(normalFont),
      m_BoldFont(boldFont),
      m_Rows(std::max<size_t>(1, height / normalFont->getHeight())),
      m_Cols(std::max<size_t>(1, width / normalFont->getWidth())),
      m_OffsetLeft(offsetLeft),
      m_OffsetTop(offsetTop),
      m_Cursor({0, 0}),
      m_CursorVisible(false),
      m_CursorFilled(true),
      m_Rendering(false),
      m_ActiveRect(nullptr) {
  m_Vterm = vterm_new(static_cast<int>(m_Rows), static_cast<int>(m_Cols));
  if (!m_Vterm)
    return;
  vterm_set_utf8(m_Vterm, 1);
  m_Screen = vterm_obtain_screen(m_Vterm);
  m_State = vterm_obtain_state(m_Vterm);

  static const VTermScreenCallbacks callbacks = {
      damageCallback,         moveRectCallback,      moveCursorCallback,
      setTermPropCallback,    bellCallback,          resizeCallback,
      scrollbackPushCallback, scrollbackPopCallback, scrollbackClearCallback};
  vterm_screen_set_callbacks(m_Screen, &callbacks, this);
  vterm_screen_enable_reflow(m_Screen, true);
  vterm_screen_enable_altscreen(m_Screen, true);
  vterm_screen_set_damage_merge(m_Screen, VTERM_DAMAGE_SCROLL);
  vterm_output_set_callback(m_Vterm, outputCallback, this);

  // Reset emits the initial full-screen damage notification. The callback
  // mirrors cells into this buffer, so it must exist before reset runs.
  m_Cells.resize(m_Rows * m_Cols);
  std::memset(m_Cells.data(), 0, m_Cells.size() * sizeof(m_Cells[0]));
  vterm_screen_reset(m_Screen, 1);
}

Vterm::~Vterm() {
  if (m_Vterm)
    vterm_free(m_Vterm);
}

void Vterm::write(const char* bytes, size_t length, DirtyRectangle& rect) {
  if (!m_Vterm || !bytes || !length)
    return;

  m_ActiveRect = &rect;
  vterm_input_write(m_Vterm, bytes, length);
  vterm_screen_flush_damage(m_Screen);
  renderPending(rect);
  m_ActiveRect = nullptr;
}

void Vterm::renderAll(DirtyRectangle& rect) {
  if (!m_Screen)
    return;

  VTermRect full = {0, static_cast<int>(m_Rows), 0, static_cast<int>(m_Cols)};
  syncRect(full);
  for (int row = 0; row < static_cast<int>(m_Rows); ++row)
    for (int col = 0; col < static_cast<int>(m_Cols); ++col)
      renderCell(rect, row, col, false);
  m_PendingDamage.clear();
}

void Vterm::processKey(uint64_t key) {
  VTermModifier modifiers = VTERM_MOD_NONE;
  if (key & Keyboard::Shift)
    modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_SHIFT);
  if (key & Keyboard::Alt)
    modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_ALT);
  if (key & Keyboard::Ctrl)
    modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_CTRL);

  if (key & Keyboard::Special) {
    const char* name = reinterpret_cast<const char*>(&key);
    VTermKey special = VTERM_KEY_NONE;
    if (!std::strncmp(name, "left", 4))
      special = VTERM_KEY_LEFT;
    else if (!std::strncmp(name, "right", 5))
      special = VTERM_KEY_RIGHT;
    else if (!std::strncmp(name, "up", 2))
      special = VTERM_KEY_UP;
    else if (!std::strncmp(name, "down", 4))
      special = VTERM_KEY_DOWN;
    else if (!std::strncmp(name, "home", 4))
      special = VTERM_KEY_HOME;
    else if (!std::strncmp(name, "end", 3))
      special = VTERM_KEY_END;
    else if (!std::strncmp(name, "insert", 6))
      special = VTERM_KEY_INS;
    else if (!std::strncmp(name, "delete", 6))
      special = VTERM_KEY_DEL;
    else if (!std::strncmp(name, "pageup", 6))
      special = VTERM_KEY_PAGEUP;
    else if (!std::strncmp(name, "pagedown", 8))
      special = VTERM_KEY_PAGEDOWN;
    else if (!std::strncmp(name, "backspace", 9))
      special = VTERM_KEY_BACKSPACE;
    else if (!std::strncmp(name, "tab", 3))
      special = VTERM_KEY_TAB;
    else if (!std::strncmp(name, "enter", 5))
      special = VTERM_KEY_ENTER;

    if (special != VTERM_KEY_NONE)
      vterm_keyboard_key(m_Vterm, special, modifiers);
    return;
  }

  uint32_t codepoint = static_cast<uint32_t>(key & 0xFFFFFFFFU);
  if (codepoint <= 0x10FFFF)
    vterm_keyboard_unichar(m_Vterm, codepoint, modifiers);
}

void Vterm::resize(size_t width, size_t height, PedigreeGraphics::Framebuffer* framebuffer) {
  m_Framebuffer = framebuffer;
  int rows = static_cast<int>(std::max<size_t>(1, height / m_NormalFont->getHeight()));
  int cols = static_cast<int>(std::max<size_t>(1, width / m_NormalFont->getWidth()));
  if (rows == static_cast<int>(m_Rows) && cols == static_cast<int>(m_Cols))
    return;

  vterm_set_size(m_Vterm, rows, cols);
  m_Rows = rows;
  m_Cols = cols;
  m_Cells.resize(m_Rows * m_Cols);
  m_PendingDamage.clear();
}

void Vterm::showCursor(DirtyRectangle& rect) {
  m_CursorVisible = true;
  renderCell(rect, m_Cursor.row, m_Cursor.col, true);
}

void Vterm::hideCursor(DirtyRectangle& rect) {
  if (!m_CursorVisible)
    return;
  m_CursorVisible = false;
  renderCell(rect, m_Cursor.row, m_Cursor.col, false);
}

void Vterm::setCairo(cairo_t* cairo, cairo_surface_t* surface) {
  m_Cairo = cairo;
  m_CairoSurface = surface;
}

void Vterm::setFonts(Font* normalFont, Font* boldFont) {
  m_NormalFont = normalFont;
  m_BoldFont = boldFont;
}

void Vterm::outputCallback(const char* bytes, size_t length, void* user) {
  static_cast<Vterm*>(user)->sendInput(bytes, length);
}

int Vterm::damageCallback(VTermRect rect, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  terminal->syncRect(rect);
  terminal->queueDamage(rect);
  return 1;
}

int Vterm::moveRectCallback(VTermRect dest, VTermRect src, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  int rows = dest.end_row - dest.start_row;
  int cols = dest.end_col - dest.start_col;
  if (rows <= 0 || cols <= 0)
    return 1;

  int rowStep = dest.start_row > src.start_row ? -1 : 1;
  int first = rowStep > 0 ? 0 : rows - 1;
  for (int i = first;; i += rowStep) {
    int destRow = dest.start_row + i;
    int srcRow = src.start_row + i;
    std::memmove(&terminal->m_Cells[destRow * terminal->m_Cols + dest.start_col],
                 &terminal->m_Cells[srcRow * terminal->m_Cols + src.start_col],
                 cols * sizeof(VTermScreenCell));
    if (i == (rowStep > 0 ? rows - 1 : 0))
      break;
  }
  if (!terminal->blitMovedRect(dest, src))
    terminal->queueDamage(dest);
  return 1;
}

int Vterm::moveCursorCallback(VTermPos pos, VTermPos oldPos, int visible, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  terminal->m_Cursor = pos;
  terminal->m_CursorVisible = visible != 0;
  terminal->queueDamage({oldPos.row, oldPos.row + 1, oldPos.col, oldPos.col + 1});
  terminal->queueDamage({pos.row, pos.row + 1, pos.col, pos.col + 1});
  return 1;
}

int Vterm::setTermPropCallback(VTermProp prop, VTermValue* value, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  if (prop == VTERM_PROP_TITLE && terminal->m_Widget && value) {
    terminal->m_Widget->setTitle(
        std::string(value->string.str ? value->string.str : "", value->string.len));
  }
  return 1;
}

int Vterm::bellCallback(void*) {
  return 1;
}

int Vterm::resizeCallback(int rows, int cols, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  terminal->m_Rows = static_cast<size_t>(std::max(1, rows));
  terminal->m_Cols = static_cast<size_t>(std::max(1, cols));
  terminal->m_Cells.resize(terminal->m_Rows * terminal->m_Cols);
  std::memset(terminal->m_Cells.data(), 0, terminal->m_Cells.size() * sizeof(terminal->m_Cells[0]));
  return 1;
}

int Vterm::scrollbackPushCallback(int cols, const VTermScreenCell* cells, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  std::vector<VTermScreenCell> line(cells, cells + cols);
  if (terminal->m_Scrollback.size() == MaxScrollback)
    terminal->m_Scrollback.erase(terminal->m_Scrollback.begin());
  terminal->m_Scrollback.push_back(std::move(line));
  return 1;
}

int Vterm::scrollbackPopCallback(int cols, VTermScreenCell* cells, void* user) {
  Vterm* terminal = static_cast<Vterm*>(user);
  if (terminal->m_Scrollback.empty())
    return 0;
  std::vector<VTermScreenCell> line = std::move(terminal->m_Scrollback.back());
  terminal->m_Scrollback.pop_back();
  std::memset(cells, 0, cols * sizeof(cells[0]));
  std::memcpy(cells, line.data(), std::min<size_t>(cols, line.size()) * sizeof(cells[0]));
  return 1;
}

int Vterm::scrollbackClearCallback(void* user) {
  static_cast<Vterm*>(user)->m_Scrollback.clear();
  return 1;
}

void Vterm::syncRect(VTermRect rect) {
  rect.start_row = std::max(0, rect.start_row);
  rect.start_col = std::max(0, rect.start_col);
  rect.end_row = std::min(static_cast<int>(m_Rows), rect.end_row);
  rect.end_col = std::min(static_cast<int>(m_Cols), rect.end_col);
  for (int row = rect.start_row; row < rect.end_row; ++row)
    for (int col = rect.start_col; col < rect.end_col; ++col)
      vterm_screen_get_cell(m_Screen, {row, col}, &m_Cells[row * m_Cols + col]);
}

void Vterm::queueDamage(VTermRect rect) {
  if (rect.start_row >= rect.end_row || rect.start_col >= rect.end_col)
    return;
  for (VTermRect& pending : m_PendingDamage) {
    if (intersectsOrTouches(pending, rect)) {
      pending.start_row = std::min(pending.start_row, rect.start_row);
      pending.start_col = std::min(pending.start_col, rect.start_col);
      pending.end_row = std::max(pending.end_row, rect.end_row);
      pending.end_col = std::max(pending.end_col, rect.end_col);
      return;
    }
  }
  m_PendingDamage.push_back(rect);
}

void Vterm::renderPending(DirtyRectangle& rect) {
  if (m_Rendering)
    return;
  m_Rendering = true;
  for (const VTermRect& damage : m_PendingDamage) {
    for (int row = damage.start_row; row < damage.end_row; ++row)
      for (int col = damage.start_col; col < damage.end_col; ++col)
        renderCell(rect, row, col, m_CursorVisible && row == m_Cursor.row && col == m_Cursor.col);
  }
  m_PendingDamage.clear();
  m_Rendering = false;
}

bool Vterm::blitMovedRect(VTermRect dest, VTermRect src) {
  if (!m_ActiveRect || !m_Cairo || !m_CairoSurface || !m_NormalFont)
    return false;

  size_t cellWidth = m_NormalFont->getWidth();
  size_t cellHeight = m_NormalFont->getHeight();
  double sourceX = m_OffsetLeft + static_cast<size_t>(src.start_col) * cellWidth;
  double sourceY = m_OffsetTop + static_cast<size_t>(src.start_row) * cellHeight;
  double destinationX = m_OffsetLeft + static_cast<size_t>(dest.start_col) * cellWidth;
  double destinationY = m_OffsetTop + static_cast<size_t>(dest.start_row) * cellHeight;
  double width = static_cast<size_t>(dest.end_col - dest.start_col) * cellWidth;
  double height = static_cast<size_t>(dest.end_row - dest.start_row) * cellHeight;

  // The group keeps a scroll from sampling pixels already overwritten by the
  // destination when the source and destination overlap.
  cairo_save(m_Cairo);
  cairo_push_group(m_Cairo);
  cairo_set_operator(m_Cairo, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(m_Cairo, m_CairoSurface, destinationX - sourceX, destinationY - sourceY);
  cairo_rectangle(m_Cairo, destinationX, destinationY, width, height);
  cairo_fill(m_Cairo);
  cairo_pop_group_to_source(m_Cairo);
  cairo_set_operator(m_Cairo, CAIRO_OPERATOR_SOURCE);
  cairo_rectangle(m_Cairo, destinationX, destinationY, width, height);
  cairo_fill(m_Cairo);
  cairo_restore(m_Cairo);

  m_ActiveRect->point(static_cast<size_t>(destinationX), static_cast<size_t>(destinationY));
  m_ActiveRect->point(static_cast<size_t>(destinationX + width),
                      static_cast<size_t>(destinationY + height));
  return true;
}

void Vterm::renderCell(DirtyRectangle& rect, int row, int col, bool cursor) {
  if (row < 0 || col < 0 || row >= static_cast<int>(m_Rows) || col >= static_cast<int>(m_Cols))
    return;
  VTermScreenCell& cell = m_Cells[row * m_Cols + col];
  uint32_t foreground = color(cell.fg, true);
  uint32_t background = color(cell.bg, false);
  uint8_t flags = 0;
  flags |= cell.attrs.bold ? RenderBold : 0;
  flags |= cell.attrs.underline ? RenderUnderline : 0;
  flags |= cell.attrs.italic ? RenderItalic : 0;
  flags |= cell.attrs.reverse ? RenderReverse : 0;
  flags |= cell.attrs.strike ? RenderStrike : 0;
  if (flags & RenderReverse || cursor) {
    uint32_t swap = foreground;
    foreground = background;
    background = swap;
  }

  uint32_t glyph = cell.chars[0] ? cell.chars[0] : ' ';
  Font* font = (flags & RenderBold) ? m_BoldFont : m_NormalFont;
  size_t x = m_OffsetLeft + static_cast<size_t>(col) * m_NormalFont->getWidth();
  size_t y = m_OffsetTop + static_cast<size_t>(row) * m_NormalFont->getHeight();
  font->render(m_Framebuffer, glyph, x, y, foreground, background, true, (flags & RenderBold) != 0,
               (flags & RenderItalic) != 0, (flags & RenderUnderline) != 0);
  if (cursor && !m_CursorFilled && m_Cairo) {
    cairo_save(m_Cairo);
    cairo_set_operator(m_Cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(m_Cairo, ((foreground >> 16) & 0xFF) / 255.0,
                         ((foreground >> 8) & 0xFF) / 255.0, (foreground & 0xFF) / 255.0);
    cairo_rectangle(m_Cairo, x + 1, y + 1, m_NormalFont->getWidth() - 2,
                    m_NormalFont->getHeight() - 2);
    cairo_stroke(m_Cairo);
    cairo_restore(m_Cairo);
  }
  rect.point(x, y);
  rect.point(x + m_NormalFont->getWidth(), y + m_NormalFont->getHeight());
}

uint32_t Vterm::color(const VTermColor& source, bool foreground) const {
  if ((foreground && VTERM_COLOR_IS_DEFAULT_FG(&source)) ||
      (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&source)))
    return foreground ? 0xC0C0C0 : 0x000000;
  VTermColor converted = source;
  if (VTERM_COLOR_IS_INDEXED(&converted))
    vterm_screen_convert_color_to_rgb(m_Screen, &converted);
  if (VTERM_COLOR_IS_RGB(&converted))
    return (static_cast<uint32_t>(converted.rgb.red) << 16) |
           (static_cast<uint32_t>(converted.rgb.green) << 8) | converted.rgb.blue;
  return foreground ? 0xC0C0C0 : 0x000000;
}

void Vterm::sendInput(const char* bytes, size_t length) {
  if (m_Terminal)
    m_Terminal->sendInput(bytes, length);
}
