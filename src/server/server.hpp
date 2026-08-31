#pragma once

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "common/socket_utils.hpp"
#include "server/kv_store.hpp"
#include "server/worker_reactor.hpp"

namespace kv {

// Accepts connections on the calling thread -- a plain blocking accept()
// loop, since accepting is the only thing this thread ever does, there's
// no need for poll() here -- and round-robins each new connection to one
// of N WorkerReactor threads. Each reactor runs its own independent
// poll() event loop over its assigned connections, so connections are
// serviced in real parallel across threads while each individual reactor
// keeps Phase 1's single-loop-per-connection-set model. The KVStore is
// shared across all reactors and is internally synchronized.
class KvServer {
 public:
  KvServer(uint16_t port, KVStore& store, size_t num_threads);
  ~KvServer();

  KvServer(const KvServer&) = delete;
  KvServer& operator=(const KvServer&) = delete;

  // Blocks forever, accepting and dispatching connections.
  void run();

 private:
  net::Socket listen_socket_;
  std::vector<std::unique_ptr<WorkerReactor>> reactors_;
  std::vector<std::thread> reactor_threads_;
  size_t next_reactor_ = 0;
};

}  // namespace kv
