#include "libui/transport.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

namespace libui::transport {
namespace {

bool setNonBlockingFd(int fd, bool enabled) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  const int updated = enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
  return fcntl(fd, F_SETFL, updated) == 0;
}

bool makeAddress(const std::string& path, sockaddr_un& address) {
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return false;
  }
  std::memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return true;
}

bool removeStaleEndpoint(const std::string& path) {
  sockaddr_un address{};
  if (!makeAddress(path, address)) {
    return false;
  }

  const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (probe < 0) {
    return false;
  }

  const int result = ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  const int error = errno;
  ::close(probe);
  if (result == 0) {
    return false;
  }

  if (error != ECONNREFUSED && error != ENOENT && error != ENOTSOCK) {
    return false;
  }
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

}  // namespace

Connection::~Connection() { close(); }

Connection::Connection(Connection&& other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; }

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  close();
  m_fd = other.m_fd;
  other.m_fd = -1;
  return *this;
}

Connection Connection::connect(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }

  sockaddr_un address{};
  if (!makeAddress(path, address) ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ::close(fd);
    return {};
  }
  return Connection(fd);
}

void Connection::close() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
  m_fd = -1;
}

bool Connection::setNonBlocking(bool enabled) {
  return valid() && setNonBlockingFd(m_fd, enabled);
}

bool Connection::send(const std::vector<std::uint8_t>& bytes) const {
  if (!valid()) {
    return false;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::send(m_fd, bytes.data() + offset, bytes.size() - offset, 0);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

ReceiveStatus Connection::receive(std::vector<std::uint8_t>& bytes, bool waitForData) const {
  bytes.clear();
  if (!valid()) {
    return ReceiveStatus::Error;
  }

  if (waitForData) {
    pollfd descriptor{m_fd, POLLIN, 0};
    int result;
    do {
      result = ::poll(&descriptor, 1, -1);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
      return ReceiveStatus::Error;
    }
  }

  std::uint8_t buffer[16 * 1024];
  ssize_t received;
  do {
    received = ::recv(m_fd, buffer, sizeof(buffer), 0);
  } while (received < 0 && errno == EINTR);
  if (received > 0) {
    bytes.assign(buffer, buffer + received);
    return ReceiveStatus::Data;
  }
  if (received == 0) {
    return ReceiveStatus::Closed;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return ReceiveStatus::WouldBlock;
  }
  return ReceiveStatus::Error;
}

Listener::~Listener() { close(); }

Listener::Listener(Listener&& other) noexcept
    : m_fd(other.m_fd), m_path(std::move(other.m_path)) {
  other.m_fd = -1;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  close();
  m_fd = other.m_fd;
  m_path = std::move(other.m_path);
  other.m_fd = -1;
  return *this;
}

Listener Listener::bind(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }

  sockaddr_un address{};
  if (!makeAddress(path, address)) {
    ::close(fd);
    return {};
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    if (errno != EADDRINUSE || !removeStaleEndpoint(path) ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      ::close(fd);
      return {};
    }
  }
  if (::listen(fd, 8) < 0) {
    ::close(fd);
    ::unlink(path.c_str());
    return {};
  }
  return Listener(fd, path);
}

void Listener::close() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
  if (!m_path.empty()) {
    ::unlink(m_path.c_str());
  }
  m_fd = -1;
  m_path.clear();
}

bool Listener::setNonBlocking(bool enabled) {
  return valid() && setNonBlockingFd(m_fd, enabled);
}

Connection Listener::accept() const {
  if (!valid()) {
    return {};
  }
  const int fd = ::accept(m_fd, nullptr, nullptr);
  if (fd < 0) {
    return {};
  }
  return Connection(fd);
}

}  // namespace libui::transport
