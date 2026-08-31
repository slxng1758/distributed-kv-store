#include "server/server.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace kv {

namespace {

// Writes the whole response, blocking the single event-loop thread only if
// the kernel send buffer is momentarily full. No POLLOUT-based write
// buffering: response sizes here are small (bounded by kMaxValueLen) and
// the benchmark runs entirely over localhost, so a slow reader stalling the
// loop is a documented simplification, not expected in practice.
void write_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd pfd{fd, POLLOUT, 0};
      ::poll(&pfd, 1, -1);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return;  // connection broken; close_client() will clean up
  }
}

}  // namespace

KvServer::KvServer(uint16_t port, KVStore& store)
    : listen_socket_(net::create_listening_socket(port)), store_(store) {
  net::set_nonblocking(listen_socket_.fd());
  poll_fds_.push_back(pollfd{listen_socket_.fd(), POLLIN, 0});
}

void KvServer::run() {
  while (true) {
    int ready = ::poll(poll_fds_.data(), poll_fds_.size(), -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
    }

    if (poll_fds_[0].revents & POLLIN) {
      accept_new_connections();
    }

    for (size_t i = 1; i < poll_fds_.size();) {
      short revents = poll_fds_[i].revents;
      if (revents & (POLLIN | POLLHUP | POLLERR)) {
        size_t before = poll_fds_.size();
        service_client(i);
        if (poll_fds_.size() < before) {
          // close_client() swap-removed index i; re-check what's there now.
          continue;
        }
      }
      ++i;
    }
  }
}

void KvServer::accept_new_connections() {
  while (true) {
    int client_fd = ::accept(listen_socket_.fd(), nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      return;
    }
    net::set_nonblocking(client_fd);
    net::set_tcp_nodelay(client_fd);
    poll_fds_.push_back(pollfd{client_fd, POLLIN, 0});
    parsers_.emplace(client_fd, protocol::IncrementalParser{});
  }
}

void KvServer::service_client(size_t poll_index) {
  int fd = poll_fds_[poll_index].fd;
  char buf[4096];
  bool should_close = false;

  while (true) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      auto& parser = parsers_.at(fd);
      parser.feed(buf, static_cast<size_t>(n));

      protocol::Request req;
      protocol::ParseStatus status;
      while ((status = parser.try_parse_request(req)) !=
             protocol::ParseStatus::Incomplete) {
        std::string response = (status == protocol::ParseStatus::Error)
                                    ? protocol::encode_error_response(
                                          parser.error_message())
                                    : handle_request(req);
        write_all(fd, response);
      }

      if (static_cast<size_t>(n) < sizeof(buf)) break;  // drained for now
      continue;
    }
    if (n == 0) {
      should_close = true;
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    if (errno == EINTR) continue;
    should_close = true;
    break;
  }

  if (should_close) close_client(poll_index);
}

void KvServer::close_client(size_t poll_index) {
  int fd = poll_fds_[poll_index].fd;
  parsers_.erase(fd);
  ::close(fd);
  poll_fds_[poll_index] = poll_fds_.back();
  poll_fds_.pop_back();
}

std::string KvServer::handle_request(const protocol::Request& req) {
  switch (req.command) {
    case protocol::Command::Get: {
      std::string value;
      if (store_.get(req.key, value)) {
        return protocol::encode_value_response(value);
      }
      return protocol::encode_not_found_response();
    }
    case protocol::Command::Set:
      store_.set(req.key, req.value);
      return protocol::encode_ok_response();
    case protocol::Command::Delete:
      if (store_.remove(req.key)) {
        return protocol::encode_deleted_response();
      }
      return protocol::encode_not_found_response();
  }
  return protocol::encode_error_response("internal error");
}

}  // namespace kv
