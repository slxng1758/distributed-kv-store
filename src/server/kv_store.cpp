#include "server/kv_store.hpp"

namespace kv {

bool KVStore::get(const std::string& key, std::string& out) const {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  out = it->second;
  return true;
}

void KVStore::set(const std::string& key, std::string value) {
  map_[key] = std::move(value);
}

bool KVStore::remove(const std::string& key) { return map_.erase(key) > 0; }

size_t KVStore::size() const { return map_.size(); }

}  // namespace kv
