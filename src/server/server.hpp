#pragma once

#include <poll.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/protocol.hpp"
#include "common/socket_utils.hpp"
#include "server/kv_store.hpp"

namespace kv {

// Single-threaded, poll()-based TCP server. A purely iterative
// "fully handle one client, then accept the next" server would let
// whichever client connects first monopolize the server for its whole
// connection lifetime -- wrong for a benchmark that holds 50 concurrent
// persistent connections open. This interleaves many connections on one
// thread instead (the same approach single-threaded Redis uses), so the
// store is still only ever touched by one logical operation at a time.
class KvServer {
 public:
  KvServer(uint16_t port, KVStore& store);

  // Blocks forever, servicing connections until the process is killed.
  void run();

 private:
  void accept_new_connections();
  void service_client(size_t poll_index);
  void close_client(size_t poll_index);
  std::string handle_request(const protocol::Request& req);

  net::Socket listen_socket_;
  KVStore& store_;
  std::vector<pollfd> poll_fds_;  // [0] = listen socket, rest = clients
  std::unordered_map<int, protocol::IncrementalParser> parsers_;
};

}  // namespace kv
