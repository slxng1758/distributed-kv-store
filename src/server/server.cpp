#include "server/server.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace kv {

KvServer::KvServer(uint16_t port, KVStore& store, size_t num_threads)
    : listen_socket_(net::create_listening_socket(port)) {
  if (num_threads == 0) num_threads = 1;

  reactors_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    reactors_.push_back(std::make_unique<WorkerReactor>(store));
  }

  reactor_threads_.reserve(num_threads);
  for (auto& reactor : reactors_) {
    WorkerReactor* r = reactor.get();
    reactor_threads_.emplace_back([r] { r->run(); });
  }
}

KvServer::~KvServer() {
  // Reactor threads run forever (no graceful shutdown path in Phase 2 --
  // the process is always terminated externally, e.g. Ctrl-C or
  // run_benchmark.sh), so detach rather than join to avoid hanging here.
  for (auto& t : reactor_threads_) {
    if (t.joinable()) t.detach();
  }
}

void KvServer::run() {
  while (true) {
    int client_fd = ::accept(listen_socket_.fd(), nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      std::cerr << "kvserver: accept() failed: " << std::strerror(errno)
                << ", continuing\n";
      continue;
    }
    reactors_[next_reactor_]->enqueue_client(client_fd);
    next_reactor_ = (next_reactor_ + 1) % reactors_.size();
  }
}

}  // namespace kv
