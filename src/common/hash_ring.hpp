#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kv {

// FNV-1a, 64-bit. Not cryptographic -- just needs to be fast and spread
// similar keys apart, which it does well enough for routing decisions.
uint64_t fnv1a_hash(const std::string& s);

// Naive baseline for comparison only: hash(key) % num_nodes. Never used
// for actual routing (see ConsistentHashRing) -- exists so
// docs/SHARDING.md and kvhashreport can demonstrate, with numbers, why
// modulo hashing is the wrong tool for a store whose node count changes.
size_t modulo_hash_node(const std::string& key, size_t num_nodes);

// Consistent hash ring with virtual nodes. Each physical node is placed at
// virtual_nodes_per_node positions around the ring (hash of
// "<node_id>#<replica_index>" for each index); a key is routed to the
// node owning the first ring position at or after hash(key), wrapping
// around to the start if hash(key) is past every position (the ring is a
// circle, not a line -- std::map::lower_bound plus a fallback to begin()
// implements that wraparound).
//
// The property that makes this worth the extra complexity over modulo
// hashing: removing or adding one node only moves the keys that were (or
// will be) owned by that node. Every other key's owner is unchanged,
// because it never depended on the *count* of nodes, only on which ring
// positions exist between it and its neighbors.
//
// Thread-safety: read-only after construction. Build the ring once (add
// every node up front) and only call get_node() afterward and this is
// safe to share across threads without a lock -- std::map permits
// concurrent reads with no concurrent writes. add_node()/remove_node()
// are NOT safe to call concurrently with anything else.
class ConsistentHashRing {
 public:
  explicit ConsistentHashRing(size_t virtual_nodes_per_node = 150);

  void add_node(const std::string& node_id);
  void remove_node(const std::string& node_id);

  // Throws std::runtime_error if the ring has no nodes.
  std::string get_node(const std::string& key) const;

  size_t node_count() const { return nodes_.size(); }
  const std::vector<std::string>& nodes() const { return nodes_; }

 private:
  size_t virtual_nodes_per_node_;
  std::map<uint64_t, std::string> ring_;  // ring position -> node_id
  std::vector<std::string> nodes_;        // physical nodes currently present
};

}  // namespace kv
