#include "common/socket_utils.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace kv::net {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

Socket::~Socket() { close_if_valid(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close_if_valid();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

int Socket::release() {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

void Socket::close_if_valid() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) throw_errno("fcntl(F_GETFL)");
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw_errno("fcntl(F_SETFL, O_NONBLOCK)");
  }
}

void set_tcp_nodelay(int fd) {
  int one = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    throw_errno("setsockopt(TCP_NODELAY)");
  }
}

void set_recv_timeout(int fd, int timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    throw_errno("setsockopt(SO_RCVTIMEO)");
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
    throw_errno("setsockopt(SO_SNDTIMEO)");
  }
}

Socket create_listening_socket(uint16_t port, int backlog) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw_errno("socket");
  Socket sock(fd);

  int one = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    throw_errno("setsockopt(SO_REUSEADDR)");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw_errno("bind");
  }
  if (listen(fd, backlog) < 0) {
    throw_errno("listen");
  }

  return sock;
}

Socket connect_to(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* result = nullptr;
  int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
  if (rc != 0) {
    throw std::runtime_error("getaddrinfo(" + host + "): " + gai_strerror(rc));
  }

  int fd = -1;
  int last_errno = 0;
  for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      last_errno = errno;
      continue;
    }
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    last_errno = errno;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    errno = last_errno;
    throw_errno("connect to " + host + ":" + std::to_string(port));
  }

  return Socket(fd);
}

}  // namespace kv::net
