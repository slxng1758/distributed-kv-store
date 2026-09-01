#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/hash_ring.hpp"
#include "common/protocol.hpp"
#include "common/socket_utils.hpp"
#include "router/failure_detector.hpp"
#include "router/node_address.hpp"

namespace kv {

// Accepts client connections and, per key, forwards to the replica
// preference list (primary + next replication_factor-1 distinct nodes
// clockwise on the ring, see ConsistentHashRing::get_nodes) rather than a
// single node. Consistency model (see docs/REPLICATION.md for the full
// tradeoff discussion):
//   - WRITE (SET/DELETE): sent to every replica the FailureDetector
//     currently considers alive; succeeds if at least one replica
//     acknowledges. Not synchronized across replicas beyond "send to all
//     of them" -- no quorum, no versioning, no repair for a replica that
//     was down during the write.
//   - READ (GET): tried against replicas in preference-list order;
//     returns the first successful response. Backed by the "every write
//     went to every live replica" invariant above, so in the no-failure
//     steady state any live replica has the latest value.
// A node that fails mid-request is marked down immediately (reactive
// detection) so the *next* request skips straight past it instead of
// re-discovering the failure -- this is what makes failover fast rather
// than bounded by the heartbeat interval.
//
// Backend kvserver nodes are unmodified Phase 1/2/3 binaries with zero
// replication awareness. Thread-per-connection, same tradeoff as Phase 3.
class Router {
 public:
  Router(uint16_t port, std::vector<NodeAddress> nodes,
         size_t virtual_nodes_per_node, size_t replication_factor);
  ~Router();

  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // Blocks forever, accepting and dispatching client connections.
  void run();

 private:
  using BackendConns = std::unordered_map<std::string, net::Socket>;

  void handle_client(int client_fd);
  net::Socket& backend_connection(const std::string& node_id, BackendConns& conns);
  const NodeAddress* find_address(const std::string& node_id) const;

  // Fans a write out to every live replica for req.key; returns the
  // response from the first replica that acknowledged, or an Error
  // response if none did.
  protocol::Response route_write(const protocol::Request& req, BackendConns& conns);

  // Tries replicas in preference-list order; returns the first success,
  // or an Error response if every replica failed.
  protocol::Response route_read(const protocol::Request& req, BackendConns& conns);

  net::Socket listen_socket_;
  std::vector<NodeAddress> nodes_;
  ConsistentHashRing ring_;
  size_t replication_factor_;
  FailureDetector failure_detector_;
};

}  // namespace kv
