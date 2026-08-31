#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/hash_ring.hpp"
#include "common/socket_utils.hpp"

namespace kv {

struct NodeAddress {
  std::string host;
  uint16_t port = 0;
  std::string id() const { return host + ":" + std::to_string(port); }
};

// Accepts client connections and forwards each request to whichever
// backend node owns the request's key, per a consistent-hash ring built
// once at startup from --nodes and never modified afterward. Backend
// kvserver nodes are unmodified Phase 1/2 binaries with zero sharding
// awareness -- all routing logic lives here, so this is also where
// Phase 4's replication/failover logic will eventually live.
//
// Thread-per-connection: each client connection gets its own thread doing
// blocking reads/forwards/writes. Simple and correct at the benchmark's
// 50-connection scale. This is a deliberately different tradeoff from
// Phase 1/2's poll()-based multiplexing -- that lesson is already
// established, and Phase 3's teaching focus is the hashing/sharding logic,
// not another I/O model.
class Router {
 public:
  Router(uint16_t port, std::vector<NodeAddress> nodes,
         size_t virtual_nodes_per_node);

  // Blocks forever, accepting and dispatching client connections.
  void run();

 private:
  void handle_client(int client_fd);

  net::Socket listen_socket_;
  std::vector<NodeAddress> nodes_;
  ConsistentHashRing ring_;
};

}  // namespace kv
