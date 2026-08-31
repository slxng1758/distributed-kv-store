#pragma once

#include <cstdint>
#include <random>
#include <string>

#include "common/protocol.hpp"

namespace kv::bench {

// Deterministic per-worker request generator: the same seed always
// produces the same sequence of operations, so benchmark runs are
// reproducible and comparable across phases. Each benchmark worker thread
// should get its own generator seeded with (base_seed + thread_index).
class WorkloadGenerator {
 public:
  WorkloadGenerator(uint64_t seed, double get_ratio, size_t key_space,
                     size_t value_size);

  // GET requests carry no value; SET requests carry a value_size-byte
  // filler payload (payload size, not content, is what matters for
  // throughput/latency measurement).
  protocol::Request next();

 private:
  std::mt19937_64 rng_;
  std::bernoulli_distribution op_dist_;
  std::uniform_int_distribution<size_t> key_dist_;
  std::string value_template_;
};

}  // namespace kv::bench
