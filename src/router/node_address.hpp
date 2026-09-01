#pragma once

#include <cstdint>
#include <string>

namespace kv {

// Split into its own header so Router and FailureDetector can each
// depend on it without depending on each other.
struct NodeAddress {
  std::string host;
  uint16_t port = 0;
  std::string id() const { return host + ":" + std::to_string(port); }
};

}  // namespace kv
