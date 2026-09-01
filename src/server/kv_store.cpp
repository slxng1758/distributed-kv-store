#include "server/kv_store.hpp"

#include "common/hash_ring.hpp"

namespace kv {

KVStore::Shard& KVStore::shard_for(const std::string& key) {
  return shards_[fnv1a_hash(key) % kShardCount];
}

const KVStore::Shard& KVStore::shard_for(const std::string& key) const {
  return shards_[fnv1a_hash(key) % kShardCount];
}

bool KVStore::get(const std::string& key, std::string& out) const {
  const Shard& shard = shard_for(key);
  std::shared_lock lock(shard.mutex);
  auto it = shard.map.find(key);
  if (it == shard.map.end()) return false;
  out = it->second;
  return true;
}

void KVStore::set(const std::string& key, std::string value) {
  Shard& shard = shard_for(key);
  std::unique_lock lock(shard.mutex);
  auto it = shard.map.find(key);
  if (it != shard.map.end()) {
    it->second = std::move(value);
    return;
  }
  shard.map.emplace(key, std::move(value));
  size_.fetch_add(1, std::memory_order_relaxed);
}

bool KVStore::remove(const std::string& key) {
  Shard& shard = shard_for(key);
  std::unique_lock lock(shard.mutex);
  if (shard.map.erase(key) == 0) return false;
  size_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

size_t KVStore::size() const { return size_.load(std::memory_order_relaxed); }

}  // namespace kv
