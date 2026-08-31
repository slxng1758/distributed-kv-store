#include "benchmark/workload.hpp"

namespace kv::bench {

WorkloadGenerator::WorkloadGenerator(uint64_t seed, double get_ratio,
                                      size_t key_space, size_t value_size)
    : rng_(seed),
      op_dist_(get_ratio),
      key_dist_(0, key_space > 0 ? key_space - 1 : 0),
      value_template_(value_size, 'x') {}

protocol::Request WorkloadGenerator::next() {
  protocol::Request req;
  req.key = "key:" + std::to_string(key_dist_(rng_));

  if (op_dist_(rng_)) {
    req.command = protocol::Command::Get;
  } else {
    req.command = protocol::Command::Set;
    req.value = value_template_;
  }

  return req;
}

}  // namespace kv::bench
