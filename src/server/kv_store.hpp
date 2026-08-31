#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace kv {

// In-memory key-value store. Thread-safe as of Phase 2: multiple
// WorkerReactor threads call these methods concurrently on a shared
// instance. get()/size() take a shared lock so concurrent reads don't
// block each other; set()/remove() take an exclusive lock. Phase 1's
// choice to have get() copy the value out (rather than return a reference
// into map_) is exactly what makes this safe -- a reference into the map
// could not be handed back safely once the lock is released.
//
// Note what this does NOT provide: individual operations are atomic, but
// a compound sequence like "GET key, compute new value, SET key" is not --
// two threads can race between their GET and their SET and one update can
// be silently lost. That's why real stores expose atomic primitives like
// INCR instead of making clients build read-modify-write themselves.
class KVStore {
 public:
  KVStore() = default;

  bool get(const std::string& key, std::string& out) const;
  void set(const std::string& key, std::string value);
  bool remove(const std::string& key);
  size_t size() const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::string> map_;
};

}  // namespace kv
