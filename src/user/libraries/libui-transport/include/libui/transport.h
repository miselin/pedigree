#ifndef LIBUI_TRANSPORT_H
#define LIBUI_TRANSPORT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace libui::transport {

enum class ReceiveStatus { Data, WouldBlock, Closed, Error };

class Connection {
 public:
  Connection() = default;
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;

  static Connection connect(const std::string& path);

  bool valid() const {
    return m_fd >= 0;
  }
  int descriptor() const {
    return m_fd;
  }
  void close();

  bool setNonBlocking(bool enabled);
  bool send(const std::vector<std::uint8_t>& bytes) const;
  ReceiveStatus receive(std::vector<std::uint8_t>& bytes, bool waitForData) const;

 private:
  friend class Listener;
  explicit Connection(int fd) : m_fd(fd) {}

  int m_fd = -1;
};

class Listener {
 public:
  Listener() = default;
  ~Listener();

  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;

  static Listener bind(const std::string& path);

  bool valid() const {
    return m_fd >= 0;
  }
  void close();
  bool setNonBlocking(bool enabled);
  Connection accept() const;

 private:
  Listener(int fd, std::string path) : m_fd(fd), m_path(std::move(path)) {}

  int m_fd = -1;
  std::string m_path;
};

}  // namespace libui::transport

#endif
