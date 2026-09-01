#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "router/node_address.hpp"

namespace kv {

// Tracks per-node liveness for the router via two complementary paths:
//
// 1. Background heartbeat: one thread PINGs every node on a fixed
//    interval, via a fresh connect-PING-close each cycle rather than a
//    kept-open connection -- simpler to get right, and a TCP handshake on
//    localhost is cheap enough at a several-hundred-ms interval that it's
//    not worth the extra state of managing reconnects on a persistent
//    socket. A bounded receive timeout (set_recv_timeout) means an
//    unresponsive-but-not-crashed node can't hang the thread. This path
//    is what notices a node coming back up, and what proactively keeps
//    routing away from a node nobody has tried to use recently.
// 2. Reactive marking: request-handling threads call mark_down()
//    immediately when a forward to a node fails, rather than waiting for
//    the next heartbeat tick. This is what makes failover fast --
//    detection happens at the moment of actual failure, not on a fixed
//    heartbeat cadence.
//
// is_alive()/mark_down()/mark_up() are safe to call from any thread.
class FailureDetector {
 public:
  FailureDetector(std::vector<NodeAddress> nodes,
                   std::chrono::milliseconds interval,
                   std::chrono::milliseconds timeout);
  ~FailureDetector();

  FailureDetector(const FailureDetector&) = delete;
  FailureDetector& operator=(const FailureDetector&) = delete;

  void start();
  void stop();

  bool is_alive(const std::string& node_id) const;
  void mark_down(const std::string& node_id);
  void mark_up(const std::string& node_id);

 private:
  void heartbeat_loop();
  bool ping_once(const NodeAddress& addr);

  std::vector<NodeAddress> nodes_;
  std::chrono::milliseconds interval_;
  std::chrono::milliseconds timeout_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, bool> alive_;

  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace kv
