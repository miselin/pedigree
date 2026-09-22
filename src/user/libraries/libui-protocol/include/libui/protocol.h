#ifndef LIBUI_PROTOCOL_H
#define LIBUI_PROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.pb.h"

namespace libui::protocol {

inline constexpr std::uint32_t Magic = 0x4c554931;
inline constexpr std::uint16_t Version = 3;

enum class Opcode : std::uint16_t {
  CreateWindow = 1,
  CreateWindowReply = 2,
  DestroyWindow = 3,
  AttachBuffer = 4,
  Present = 5,
  SetTitle = 6,
  Configure = 7,
  Close = 8,
  Input = 9,
  Error = 10,
  AttachBufferReply = 11,
  PresentReply = 12,
  ReleaseBuffer = 13,
  ReleaseBufferReply = 14,
  GenerationRetired = 15,
  ResizeWindow = 16,
  ResizeWindowReply = 17,
  PopupDismiss = 18,
};

enum class WindowSizeMode : std::uint8_t { Outer = 0, Client = 1 };

enum class ResultCode : std::uint8_t {
  Ok,
  InvalidWindow,
  InvalidBuffer,
  InvalidGeneration,
  InvalidSize,
  Busy,
};

enum WindowStyleFlags : std::uint32_t {
  StyleCaption = 1u << 0,
  StyleSystemMenu = 1u << 1,
  StyleMinimizeBox = 1u << 2,
  StyleMaximizeBox = 1u << 3,
  StyleThickFrame = 1u << 4,
  StyleBorder = 1u << 5,
  StylePopup = 1u << 6,
  // The client paints a scrollbar corner that doubles as a window resize grip.
  StyleClientResizeGrip = 1u << 7,
};

inline constexpr std::uint32_t StandardWindowStyle = StyleCaption | StyleSystemMenu |
                                                     StyleMinimizeBox | StyleMaximizeBox |
                                                     StyleThickFrame | StyleBorder;

struct Message {
  Opcode opcode = Opcode::Error;
  std::uint32_t requestId = 0;
  std::vector<std::uint8_t> payload;
};

using CreateWindowRequest = wire::CreateWindowRequest;
using CreateWindowReply = wire::CreateWindowReply;
using DestroyWindowRequest = wire::DestroyWindowRequest;
using ResizeWindowRequest = wire::ResizeWindowRequest;
using BufferDescriptor = wire::BufferDescriptor;
using PresentRequest = wire::PresentRequest;
using ReleaseBufferRequest = wire::ReleaseBufferRequest;
using ResultReply = wire::ResultReply;
using ConfigureEvent = wire::ConfigureEvent;
using CloseEvent = wire::CloseEvent;
using PopupDismissEvent = wire::PopupDismissEvent;
using GenerationRetiredEvent = wire::GenerationRetiredEvent;
using InputEvent = wire::InputEvent;
using PointerInput = wire::PointerInput;
using KeyInput = wire::KeyInput;

std::vector<std::uint8_t> encode(const Message& message);

class Decoder {
 public:
  bool feed(const std::vector<std::uint8_t>& bytes, std::vector<Message>& messages);

 private:
  std::vector<std::uint8_t> m_pending;
};

Message makeCreateWindow(std::uint32_t requestId, const CreateWindowRequest& request);
bool readCreateWindow(const Message& message, CreateWindowRequest& request);
Message makeCreateWindowReply(std::uint32_t requestId, const CreateWindowReply& reply);
bool readCreateWindowReply(const Message& message, CreateWindowReply& reply);
Message makeDestroyWindow(std::uint32_t requestId, std::uint32_t windowId);
bool readWindowId(const Message& message, std::uint32_t& windowId);
Message makeResizeWindow(std::uint32_t requestId, const ResizeWindowRequest& request);
bool readResizeWindow(const Message& message, ResizeWindowRequest& request);
Message makeAttachBuffer(std::uint32_t requestId, const BufferDescriptor& buffer);
bool readAttachBuffer(const Message& message, BufferDescriptor& buffer);
Message makeResultReply(Opcode opcode, std::uint32_t requestId, ResultCode result);
bool readResultReply(const Message& message, Opcode opcode, ResultCode& result);
Message makePresent(std::uint32_t requestId, std::uint32_t windowId, std::uint32_t bufferId,
                    std::uint64_t generation);
bool readPresent(const Message& message, std::uint32_t& windowId, std::uint32_t& bufferId,
                 std::uint64_t& generation);
Message makeReleaseBuffer(std::uint32_t requestId, std::uint32_t windowId, std::uint32_t bufferId,
                          std::uint64_t generation);
bool readReleaseBuffer(const Message& message, std::uint32_t& windowId, std::uint32_t& bufferId,
                       std::uint64_t& generation);
Message makeConfigure(std::uint32_t windowId, std::int32_t outerX, std::int32_t outerY,
                      std::int32_t outerWidth, std::int32_t outerHeight, std::int32_t clientWidth,
                      std::int32_t clientHeight);
bool readConfigure(const Message& message, ConfigureEvent& event);
Message makeClose(std::uint32_t windowId);
bool readClose(const Message& message, CloseEvent& event);
Message makePopupDismiss(std::uint32_t windowId);
bool readPopupDismiss(const Message& message, PopupDismissEvent& event);
Message makeGenerationRetired(std::uint32_t windowId, std::uint64_t generation);
bool readGenerationRetired(const Message& message, GenerationRetiredEvent& event);
Message makePointerInput(std::uint32_t windowId, wire::PointerInputType type, std::uint32_t button,
                         std::uint32_t buttonBitmap, std::int32_t x, std::int32_t y);
Message makeKeyInput(std::uint32_t windowId, bool isDown, std::uint32_t scancode,
                     std::int32_t keycode, std::uint32_t modifiers);
Message makeTextInput(std::uint32_t windowId, const std::string& text);
bool readInput(const Message& message, InputEvent& event);

}  // namespace libui::protocol

#endif
