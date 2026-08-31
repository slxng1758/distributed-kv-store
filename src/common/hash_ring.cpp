#include "common/hash_ring.hpp"

#include <algorithm>
#include <stdexcept>

namespace kv {

namespace {

// MurmurHash3's 64-bit finalizer. Plain FNV-1a has weak avalanche: for
// strings that share a long common prefix and differ only in a late byte
// (exactly what "<node_id>#<replica_index>" looks like for replica
// indices 0..149), its high-order output bits barely change -- verified
// empirically here: fnv1a_hash("nodeA#0") through fnv1a_hash("nodeA#7")
// all share the same top byte, which is disastrous for a ring keyed by
// std::map<uint64_t, ...> ordering, since virtual nodes end up clustered
// instead of spread around the ring. Running the raw FNV-1a output
// through this finalizer restores proper avalanche at negligible cost.
uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

}  // namespace

uint64_t fnv1a_hash(const std::string& s) {
  uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;  // FNV prime
  }
  return fmix64(hash);
}

size_t modulo_hash_node(const std::string& key, size_t num_nodes) {
  return static_cast<size_t>(fnv1a_hash(key) % num_nodes);
}

ConsistentHashRing::ConsistentHashRing(size_t virtual_nodes_per_node)
    : virtual_nodes_per_node_(virtual_nodes_per_node) {}

void ConsistentHashRing::add_node(const std::string& node_id) {
  for (size_t i = 0; i < virtual_nodes_per_node_; ++i) {
    uint64_t pos = fnv1a_hash(node_id + "#" + std::to_string(i));
    ring_[pos] = node_id;
  }
  nodes_.push_back(node_id);
}

void ConsistentHashRing::remove_node(const std::string& node_id) {
  for (size_t i = 0; i < virtual_nodes_per_node_; ++i) {
    uint64_t pos = fnv1a_hash(node_id + "#" + std::to_string(i));
    ring_.erase(pos);
  }
  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node_id), nodes_.end());
}

std::string ConsistentHashRing::get_node(const std::string& key) const {
  if (ring_.empty()) {
    throw std::runtime_error("ConsistentHashRing::get_node: ring has no nodes");
  }
  uint64_t pos = fnv1a_hash(key);
  auto it = ring_.lower_bound(pos);
  if (it == ring_.end()) it = ring_.begin();  // wrap around the ring
  return it->second;
}

}  // namespace kv
