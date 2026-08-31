#pragma once

#include <cstdint>
#include <string>

namespace kv::net {

// RAII wrapper around a POSIX socket file descriptor; closes on destruction.
class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  // Relinquishes ownership of the fd to the caller (e.g. to hand it to a
  // poll()-driven event loop that manages lifetime itself).
  int release();

 private:
  void close_if_valid();
  int fd_ = -1;
};

// Sets O_NONBLOCK on fd. Throws std::runtime_error on failure.
void set_nonblocking(int fd);

// Disables Nagle's algorithm. Without this, small request/response
// round trips can pick up tens of ms of spurious latency from Nagle +
// delayed ACKs, which would corrupt latency benchmarks. Throws
// std::runtime_error on failure.
void set_tcp_nodelay(int fd);

// Creates, binds (SO_REUSEADDR), and listens on a TCP socket bound to
// 0.0.0.0:port. Throws std::runtime_error on failure.
Socket create_listening_socket(uint16_t port, int backlog = 128);

// Resolves host and makes a blocking connection to host:port. Throws
// std::runtime_error on failure (including DNS resolution failure).
Socket connect_to(const std::string& host, uint16_t port);

}  // namespace kv::net
