#pragma once

#include <array>
#include <atomic>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace kv {

// In-memory key-value store, partitioned into kShardCount independent
// shards -- each with its own map and its own shared_mutex -- so
// operations on different keys never contend for the same lock. This
// replaces Phase 2's original design (one global shared_mutex over the
// whole map), which a benchmark showed was the actual bottleneck: with a
// single lock, every thread serializes on the same mutex regardless of
// which keys it touches, and for an operation this cheap (a hash-map
// lookup), that synchronization overhead can exceed the parallelism
// gained (see docs/CONCURRENCY.md for the measured before/after). Sharding
// the lock -- the same principle Java's ConcurrentHashMap and Redis
// Cluster apply -- means two threads touching different keys can now run
// fully in parallel.
//
// get() still copies the value out rather than returning a reference (see
// Phase 1's rationale, unchanged): a reference into a shard's map could
// not be handed back safely once that shard's lock is released.
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
  // A small power of two comfortably larger than the default --threads
  // count (4), so concurrent operations from different reactor threads
  // usually land on different shards.
  static constexpr size_t kShardCount = 16;

  struct Shard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::string> map;
  };

  Shard& shard_for(const std::string& key);
  const Shard& shard_for(const std::string& key) const;

  std::array<Shard, kShardCount> shards_;
  // Tracked separately (rather than summing shard sizes under N locks) so
  // size() is exact and lock-free.
  std::atomic<size_t> size_{0};
};

}  // namespace kv
