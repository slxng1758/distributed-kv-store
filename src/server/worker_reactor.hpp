#pragma once

#include <poll.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/protocol.hpp"
#include "server/kv_store.hpp"

namespace kv {

// One poll()-based reactor running on its own thread, owning a disjoint
// subset of client connections. Multiple reactors share one KVStore (now
// internally synchronized -- see kv_store.hpp); within a single reactor,
// the same "one request in flight per connection at a time, interleaved
// across many sockets" model from Phase 1 still holds. Concurrency comes
// from running N such reactors in parallel on N threads, not from making
// any single reactor's own loop concurrent.
//
// New connections arrive from a different thread (KvServer's accept loop),
// so they can't just be pushed onto poll_fds_ directly -- enqueue_client()
// stages the fd behind a mutex and wakes this reactor's blocked poll() via
// a self-pipe (macOS has no eventfd, so this is the portable equivalent).
class WorkerReactor {
 public:
  explicit WorkerReactor(KVStore& store);
  ~WorkerReactor();

  WorkerReactor(const WorkerReactor&) = delete;
  WorkerReactor& operator=(const WorkerReactor&) = delete;

  // Thread-safe. Called from the accept thread to hand off a freshly
  // accepted client fd; this reactor takes ownership of it.
  void enqueue_client(int fd);

  // Blocks forever servicing this reactor's connections.
  void run();

 private:
  void drain_wakeup_pipe();
  void accept_pending_clients();
  void service_client(size_t poll_index);
  void close_client(size_t poll_index);
  std::string handle_request(const protocol::Request& req);

  KVStore& store_;
  int wakeup_read_fd_;
  int wakeup_write_fd_;
  std::vector<pollfd> poll_fds_;  // [0] = wakeup pipe read end, rest = clients
  std::unordered_map<int, protocol::IncrementalParser> parsers_;

  std::mutex pending_mutex_;
  std::vector<int> pending_clients_;
};

}  // namespace kv
