#pragma once

#include <string>
#include <unordered_map>

namespace kv {

// In-memory key-value store. Not thread-safe: Phase 1's server touches it
// from a single event-loop thread only. get() deliberately copies the value
// out (rather than returning a reference into the map) because a future
// concurrent version cannot safely hand back a reference after releasing a
// lock -- this interface is designed for the locking it doesn't have yet.
class KVStore {
 public:
  KVStore() = default;

  bool get(const std::string& key, std::string& out) const;
  void set(const std::string& key, std::string value);
  bool remove(const std::string& key);
  size_t size() const;

 private:
  std::unordered_map<std::string, std::string> map_;
};

}  // namespace kv
