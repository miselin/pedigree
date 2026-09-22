#include "libui/protocol.h"

#include <cstddef>
#include <utility>

namespace libui::protocol {
namespace {

constexpr std::size_t HeaderSize = 16;
constexpr std::uint32_t MaxPayload = 1024 * 1024;

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

bool get16(const std::vector<std::uint8_t>& in, std::size_t& offset, std::uint16_t& value) {
  if (offset + 2 > in.size()) {
    return false;
  }
  value = static_cast<std::uint16_t>(in[offset]) | static_cast<std::uint16_t>(in[offset + 1]) << 8;
  offset += 2;
  return true;
}

bool get32(const std::vector<std::uint8_t>& in, std::size_t& offset, std::uint32_t& value) {
  if (offset + 4 > in.size()) {
    return false;
  }
  value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(in[offset++]) << shift;
  }
  return true;
}

Message simple(Opcode opcode, std::uint32_t requestId) {
  return {opcode, requestId, {}};
}

template <typename Proto>
Message makeProto(Opcode opcode, std::uint32_t requestId, const Proto& payload) {
  std::string bytes;
  Message message = simple(opcode, requestId);
  if (!payload.SerializeToString(&bytes)) {
    return simple(Opcode::Error, requestId);
  }
  message.payload.assign(bytes.begin(), bytes.end());
  return message;
}

template <typename Proto>
bool readProto(const Message& message, Opcode opcode, Proto& payload) {
  if (message.opcode != opcode || message.payload.size() > MaxPayload) {
    return false;
  }
  return payload.ParseFromArray(message.payload.empty() ? nullptr : message.payload.data(),
                                static_cast<int>(message.payload.size()));
}

wire::ResultCode toWire(ResultCode result) {
  switch (result) {
    case ResultCode::Ok:
      return wire::RESULT_OK;
    case ResultCode::InvalidWindow:
      return wire::RESULT_INVALID_WINDOW;
    case ResultCode::InvalidBuffer:
      return wire::RESULT_INVALID_BUFFER;
    case ResultCode::InvalidGeneration:
      return wire::RESULT_INVALID_GENERATION;
    case ResultCode::InvalidSize:
      return wire::RESULT_INVALID_SIZE;
    case ResultCode::Busy:
      return wire::RESULT_BUSY;
  }
  return wire::RESULT_OK;
}

bool fromWire(wire::ResultCode wireResult, ResultCode& result) {
  switch (wireResult) {
    case wire::RESULT_OK:
      result = ResultCode::Ok;
      return true;
    case wire::RESULT_INVALID_WINDOW:
      result = ResultCode::InvalidWindow;
      return true;
    case wire::RESULT_INVALID_BUFFER:
      result = ResultCode::InvalidBuffer;
      return true;
    case wire::RESULT_INVALID_GENERATION:
      result = ResultCode::InvalidGeneration;
      return true;
    case wire::RESULT_INVALID_SIZE:
      result = ResultCode::InvalidSize;
      return true;
    case wire::RESULT_BUSY:
      result = ResultCode::Busy;
      return true;
    default:
      return false;
  }
}

}  // namespace

std::vector<std::uint8_t> encode(const Message& message) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(HeaderSize + message.payload.size());
  put32(bytes, Magic);
  put16(bytes, Version);
  put16(bytes, static_cast<std::uint16_t>(message.opcode));
  put32(bytes, static_cast<std::uint32_t>(message.payload.size()));
  put32(bytes, message.requestId);
  bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
  return bytes;
}

bool Decoder::feed(const std::vector<std::uint8_t>& bytes, std::vector<Message>& messages) {
  m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
  while (m_pending.size() >= HeaderSize) {
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t opcode = 0;
    std::uint32_t payloadSize = 0;
    std::uint32_t requestId = 0;
    if (!get32(m_pending, offset, magic) || !get16(m_pending, offset, version) ||
        !get16(m_pending, offset, opcode) || !get32(m_pending, offset, payloadSize) ||
        !get32(m_pending, offset, requestId)) {
      return false;
    }
    if (magic != Magic || version != Version || payloadSize > MaxPayload ||
        m_pending.size() < HeaderSize + payloadSize) {
      return magic == Magic && version == Version && payloadSize <= MaxPayload;
    }

    Message message;
    message.opcode = static_cast<Opcode>(opcode);
    message.requestId = requestId;
    message.payload.assign(m_pending.begin() + HeaderSize,
                           m_pending.begin() + HeaderSize + payloadSize);
    messages.push_back(std::move(message));
    m_pending.erase(m_pending.begin(), m_pending.begin() + HeaderSize + payloadSize);
  }
  return true;
}

Message makeCreateWindow(std::uint32_t requestId, const CreateWindowRequest& request) {
  return makeProto(Opcode::CreateWindow, requestId, request);
}

bool readCreateWindow(const Message& message, CreateWindowRequest& request) {
  return readProto(message, Opcode::CreateWindow, request);
}

Message makeCreateWindowReply(std::uint32_t requestId, const CreateWindowReply& reply) {
  return makeProto(Opcode::CreateWindowReply, requestId, reply);
}

bool readCreateWindowReply(const Message& message, CreateWindowReply& reply) {
  return readProto(message, Opcode::CreateWindowReply, reply);
}

Message makeDestroyWindow(std::uint32_t requestId, std::uint32_t windowId) {
  DestroyWindowRequest request;
  request.set_window_id(windowId);
  return makeProto(Opcode::DestroyWindow, requestId, request);
}

bool readWindowId(const Message& message, std::uint32_t& windowId) {
  DestroyWindowRequest request;
  if (!readProto(message, Opcode::DestroyWindow, request)) {
    return false;
  }
  windowId = request.window_id();
  return true;
}

Message makeResizeWindow(std::uint32_t requestId, const ResizeWindowRequest& request) {
  return makeProto(Opcode::ResizeWindow, requestId, request);
}

bool readResizeWindow(const Message& message, ResizeWindowRequest& request) {
  return readProto(message, Opcode::ResizeWindow, request);
}

Message makeAttachBuffer(std::uint32_t requestId, const BufferDescriptor& buffer) {
  return makeProto(Opcode::AttachBuffer, requestId, buffer);
}

bool readAttachBuffer(const Message& message, BufferDescriptor& buffer) {
  return readProto(message, Opcode::AttachBuffer, buffer);
}

Message makeResultReply(Opcode opcode, std::uint32_t requestId, ResultCode result) {
  ResultReply reply;
  reply.set_result(toWire(result));
  return makeProto(opcode, requestId, reply);
}

bool readResultReply(const Message& message, Opcode opcode, ResultCode& result) {
  ResultReply reply;
  return readProto(message, opcode, reply) && fromWire(reply.result(), result);
}

Message makePresent(std::uint32_t requestId, std::uint32_t windowId, std::uint32_t bufferId,
                    std::uint64_t generation) {
  PresentRequest request;
  request.set_window_id(windowId);
  request.set_buffer_id(bufferId);
  request.set_generation(generation);
  return makeProto(Opcode::Present, requestId, request);
}

bool readPresent(const Message& message, std::uint32_t& windowId, std::uint32_t& bufferId,
                 std::uint64_t& generation) {
  PresentRequest request;
  if (!readProto(message, Opcode::Present, request)) {
    return false;
  }
  windowId = request.window_id();
  bufferId = request.buffer_id();
  generation = request.generation();
  return true;
}

Message makeReleaseBuffer(std::uint32_t requestId, std::uint32_t windowId, std::uint32_t bufferId,
                          std::uint64_t generation) {
  ReleaseBufferRequest request;
  request.set_window_id(windowId);
  request.set_buffer_id(bufferId);
  request.set_generation(generation);
  return makeProto(Opcode::ReleaseBuffer, requestId, request);
}

bool readReleaseBuffer(const Message& message, std::uint32_t& windowId, std::uint32_t& bufferId,
                       std::uint64_t& generation) {
  ReleaseBufferRequest request;
  if (!readProto(message, Opcode::ReleaseBuffer, request)) {
    return false;
  }
  windowId = request.window_id();
  bufferId = request.buffer_id();
  generation = request.generation();
  return true;
}

Message makeConfigure(std::uint32_t windowId, std::int32_t outerX, std::int32_t outerY,
                      std::int32_t outerWidth, std::int32_t outerHeight, std::int32_t clientWidth,
                      std::int32_t clientHeight) {
  ConfigureEvent event;
  event.set_window_id(windowId);
  event.set_outer_x(outerX);
  event.set_outer_y(outerY);
  event.set_outer_width(outerWidth);
  event.set_outer_height(outerHeight);
  event.set_client_width(clientWidth);
  event.set_client_height(clientHeight);
  return makeProto(Opcode::Configure, 0, event);
}

bool readConfigure(const Message& message, ConfigureEvent& event) {
  return readProto(message, Opcode::Configure, event);
}

Message makeClose(std::uint32_t windowId) {
  CloseEvent event;
  event.set_window_id(windowId);
  return makeProto(Opcode::Close, 0, event);
}

bool readClose(const Message& message, CloseEvent& event) {
  return readProto(message, Opcode::Close, event);
}

Message makePopupDismiss(std::uint32_t windowId) {
  PopupDismissEvent event;
  event.set_window_id(windowId);
  return makeProto(Opcode::PopupDismiss, 0, event);
}

bool readPopupDismiss(const Message& message, PopupDismissEvent& event) {
  return readProto(message, Opcode::PopupDismiss, event);
}

Message makeGenerationRetired(std::uint32_t windowId, std::uint64_t generation) {
  GenerationRetiredEvent event;
  event.set_window_id(windowId);
  event.set_generation(generation);
  return makeProto(Opcode::GenerationRetired, 0, event);
}

bool readGenerationRetired(const Message& message, GenerationRetiredEvent& event) {
  return readProto(message, Opcode::GenerationRetired, event);
}

Message makePointerInput(std::uint32_t windowId, wire::PointerInputType type, std::uint32_t button,
                         std::uint32_t buttonBitmap, std::int32_t x, std::int32_t y) {
  InputEvent event;
  event.set_window_id(windowId);
  PointerInput* pointer = event.mutable_pointer();
  pointer->set_type(type);
  pointer->set_button_bitmap(buttonBitmap);
  pointer->set_x(x);
  pointer->set_y(y);
  pointer->set_button(button);
  return makeProto(Opcode::Input, 0, event);
}

Message makeKeyInput(std::uint32_t windowId, bool isDown, std::uint32_t scancode,
                     std::int32_t keycode, std::uint32_t modifiers) {
  InputEvent event;
  event.set_window_id(windowId);
  KeyInput* key = event.mutable_key();
  key->set_is_down(isDown);
  key->set_scancode(scancode);
  key->set_keycode(keycode);
  key->set_modifiers(modifiers);
  return makeProto(Opcode::Input, 0, event);
}

Message makeTextInput(std::uint32_t windowId, const std::string& text) {
  InputEvent event;
  event.set_window_id(windowId);
  event.mutable_text()->set_text(text);
  return makeProto(Opcode::Input, 0, event);
}

bool readInput(const Message& message, InputEvent& event) {
  return readProto(message, Opcode::Input, event) &&
         event.payload_case() != InputEvent::PAYLOAD_NOT_SET;
}

}  // namespace libui::protocol
