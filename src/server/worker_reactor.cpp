#include "server/worker_reactor.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "common/socket_utils.hpp"

namespace kv {

namespace {

// Same simplification as Phase 1: blocks this reactor thread (not the
// whole process) only if the kernel send buffer is momentarily full.
// Response sizes are small and bounded, and the benchmark runs over
// localhost, so a slow reader stalling one reactor is a documented
// tradeoff, not expected in practice.
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

WorkerReactor::WorkerReactor(KVStore& store) : store_(store) {
  int fds[2];
  if (::pipe(fds) < 0) {
    throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));
  }
  wakeup_read_fd_ = fds[0];
  wakeup_write_fd_ = fds[1];
  net::set_nonblocking(wakeup_read_fd_);
  net::set_nonblocking(wakeup_write_fd_);
  poll_fds_.push_back(pollfd{wakeup_read_fd_, POLLIN, 0});
}

WorkerReactor::~WorkerReactor() {
  // Client fds are intentionally not closed here: Phase 2 has no graceful
  // shutdown path (the process is always terminated externally -- Ctrl-C,
  // run_benchmark.sh), and the OS reclaims every fd on process exit.
  ::close(wakeup_read_fd_);
  ::close(wakeup_write_fd_);
}

void WorkerReactor::enqueue_client(int fd) {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_clients_.push_back(fd);
  }
  char byte = 1;
  ::write(wakeup_write_fd_, &byte, 1);  // best-effort wake; EAGAIN is fine
}

void WorkerReactor::run() {
  while (true) {
    int ready = ::poll(poll_fds_.data(), poll_fds_.size(), -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
    }

    if (poll_fds_[0].revents & POLLIN) {
      drain_wakeup_pipe();
      accept_pending_clients();
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

void WorkerReactor::drain_wakeup_pipe() {
  char buf[256];
  while (::read(wakeup_read_fd_, buf, sizeof(buf)) > 0) {
  }
}

void WorkerReactor::accept_pending_clients() {
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    fds.swap(pending_clients_);
  }
  for (int fd : fds) {
    net::set_nonblocking(fd);
    net::set_tcp_nodelay(fd);
    poll_fds_.push_back(pollfd{fd, POLLIN, 0});
    parsers_.emplace(fd, protocol::IncrementalParser{});
  }
}

void WorkerReactor::service_client(size_t poll_index) {
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

void WorkerReactor::close_client(size_t poll_index) {
  int fd = poll_fds_[poll_index].fd;
  parsers_.erase(fd);
  ::close(fd);
  poll_fds_[poll_index] = poll_fds_.back();
  poll_fds_.pop_back();
}

std::string WorkerReactor::handle_request(const protocol::Request& req) {
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
    case protocol::Command::Ping:
      // Pure liveness check -- never touches store_. Phase 4's failure
      // detector relies on this responding promptly and cheaply.
      return protocol::encode_pong_response();
  }
  return protocol::encode_error_response("internal error");
}

}  // namespace kv
