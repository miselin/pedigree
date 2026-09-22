#ifndef LIBUI_CORE_TYPES_H
#define LIBUI_CORE_TYPES_H

#include <cstdint>
#include <string>

namespace libui {

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  constexpr bool operator==(const Color& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
  constexpr bool operator!=(const Color& other) const {
    return !(*this == other);
  }
};

constexpr std::uint32_t packColor(Color color) {
  return (static_cast<std::uint32_t>(color.r) << 24) | (static_cast<std::uint32_t>(color.g) << 16) |
         (static_cast<std::uint32_t>(color.b) << 8) | color.a;
}

constexpr Color unpackColor(std::uint32_t color) {
  return {static_cast<std::uint8_t>(color >> 24), static_cast<std::uint8_t>(color >> 16),
          static_cast<std::uint8_t>(color >> 8), static_cast<std::uint8_t>(color)};
}

enum class PixelFormat : std::uint32_t {
  Unknown = 0,
  CairoArgb32 = 1,
  Rgba32 = 2,
  Rgb32 = 3,
  Bgr32 = 4,
};

namespace input {

using KeyCode = std::int32_t;

namespace key {
inline constexpr KeyCode Unknown = 0;
inline constexpr KeyCode Return = -1;
inline constexpr KeyCode Tab = -2;
inline constexpr KeyCode Backspace = -3;
inline constexpr KeyCode Escape = -4;
inline constexpr KeyCode Up = -5;
inline constexpr KeyCode Down = -6;
inline constexpr KeyCode Left = -7;
inline constexpr KeyCode Right = -8;
inline constexpr KeyCode Insert = -9;
inline constexpr KeyCode Delete = -10;
inline constexpr KeyCode Home = -11;
inline constexpr KeyCode End = -12;
inline constexpr KeyCode PageUp = -13;
inline constexpr KeyCode PageDown = -14;
inline constexpr KeyCode Function1 = -100;
inline constexpr KeyCode Function24 = Function1 - 23;
}  // namespace key

enum Modifier : std::uint32_t {
  NoModifiers = 0,
  Shift = 1u << 0,
  Alt = 1u << 1,
  Control = 1u << 2,
};

inline constexpr std::uint32_t LeftButton = 1;
inline constexpr std::uint32_t MiddleButton = 2;
inline constexpr std::uint32_t RightButton = 3;

enum class CursorType {
  Arrow,
  Move,
  ResizeHorizontal,
  ResizeVertical,
  ResizeNorthEastSouthWest,
  ResizeNorthWestSouthEast,
  Count,
};

struct Event {
  enum class Type {
    Quit,
    Redraw,
    PointerMove,
    PointerDown,
    PointerUp,
    KeyDown,
    KeyUp,
    TextInput,
  };

  Type type = Type::Quit;
  int x = 0;
  int y = 0;
  std::uint32_t button = 0;
  std::uint32_t buttonBitmap = 0;
  std::uint32_t scancode = 0;
  KeyCode key = key::Unknown;
  std::uint32_t modifiers = NoModifiers;
  std::string text;
};

}  // namespace input
}  // namespace libui

#endif
