#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kv::bench {

struct PercentileStats {
  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
  double mean_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;
};

// Sorts a copy of latencies_ms and computes percentiles as
// index = ceil(p * n) - 1 (clamped to [0, n-1]). This exact formula is
// documented here because different tools interpolate percentiles
// differently -- comparing numbers across benchmark phases depends on
// always using the same method.
PercentileStats compute_percentiles(std::vector<double> latencies_ms);

struct RunConfig {
  std::string host;
  uint16_t port = 0;
  size_t requests = 0;
  size_t concurrency = 0;
  double get_ratio = 0.0;
  size_t key_space = 0;
  size_t value_size = 0;
  uint64_t seed = 0;
  std::string git_rev;
};

struct RunResult {
  RunConfig config;
  double wall_clock_seconds = 0.0;
  double throughput_rps = 0.0;
  PercentileStats overall;
  PercentileStats get_stats;
  PercentileStats set_stats;
  size_t error_count = 0;
};

void print_summary(const RunResult& result);

// The stable, cross-phase comparison contract: config + throughput +
// per-op and overall latency percentiles + error count.
std::string to_json(const RunResult& result);

std::string to_csv_header();
std::string to_csv_row(double latency_ms, const std::string& op);

}  // namespace kv::bench
