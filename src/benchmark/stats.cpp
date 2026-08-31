#include "benchmark/stats.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace kv::bench {

PercentileStats compute_percentiles(std::vector<double> latencies_ms) {
  PercentileStats stats{};
  if (latencies_ms.empty()) return stats;

  std::sort(latencies_ms.begin(), latencies_ms.end());
  size_t n = latencies_ms.size();

  auto percentile = [&](double p) {
    size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n))) - 1;
    if (idx >= n) idx = n - 1;
    return latencies_ms[idx];
  };

  stats.p50_ms = percentile(0.50);
  stats.p95_ms = percentile(0.95);
  stats.p99_ms = percentile(0.99);
  stats.min_ms = latencies_ms.front();
  stats.max_ms = latencies_ms.back();

  double sum = 0.0;
  for (double v : latencies_ms) sum += v;
  stats.mean_ms = sum / static_cast<double>(n);

  return stats;
}

namespace {

std::string percentiles_json(const PercentileStats& s) {
  std::ostringstream oss;
  oss << "{\"p50\":" << s.p50_ms << ",\"p95\":" << s.p95_ms
      << ",\"p99\":" << s.p99_ms << ",\"mean\":" << s.mean_ms
      << ",\"min\":" << s.min_ms << ",\"max\":" << s.max_ms << "}";
  return oss.str();
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

void print_summary(const RunResult& r) {
  std::cout << "\n=== Benchmark Results ===\n";
  std::cout << "requests=" << r.config.requests
            << " concurrency=" << r.config.concurrency
            << " get_ratio=" << r.config.get_ratio
            << " key_space=" << r.config.key_space
            << " value_size=" << r.config.value_size
            << " seed=" << r.config.seed << "\n";
  std::cout << "wall_clock=" << r.wall_clock_seconds << "s"
            << "  throughput=" << r.throughput_rps << " req/s"
            << "  errors=" << r.error_count << "\n";

  auto print_row = [](const char* label, const PercentileStats& s) {
    std::cout << label << ": p50=" << s.p50_ms << "ms p95=" << s.p95_ms
               << "ms p99=" << s.p99_ms << "ms mean=" << s.mean_ms
               << "ms min=" << s.min_ms << "ms max=" << s.max_ms << "ms\n";
  };
  print_row("overall", r.overall);
  print_row("GET    ", r.get_stats);
  print_row("SET    ", r.set_stats);
  std::cout << std::endl;
}

std::string to_json(const RunResult& r) {
  std::ostringstream oss;
  oss << "{\n"
      << "  \"config\": {\n"
      << "    \"host\": \"" << json_escape(r.config.host) << "\",\n"
      << "    \"port\": " << r.config.port << ",\n"
      << "    \"requests\": " << r.config.requests << ",\n"
      << "    \"concurrency\": " << r.config.concurrency << ",\n"
      << "    \"get_ratio\": " << r.config.get_ratio << ",\n"
      << "    \"key_space\": " << r.config.key_space << ",\n"
      << "    \"value_size\": " << r.config.value_size << ",\n"
      << "    \"seed\": " << r.config.seed << ",\n"
      << "    \"git_rev\": \"" << json_escape(r.config.git_rev) << "\"\n"
      << "  },\n"
      << "  \"wall_clock_seconds\": " << r.wall_clock_seconds << ",\n"
      << "  \"throughput_rps\": " << r.throughput_rps << ",\n"
      << "  \"error_count\": " << r.error_count << ",\n"
      << "  \"latency_ms\": {\n"
      << "    \"overall\": " << percentiles_json(r.overall) << ",\n"
      << "    \"get\": " << percentiles_json(r.get_stats) << ",\n"
      << "    \"set\": " << percentiles_json(r.set_stats) << "\n"
      << "  }\n"
      << "}\n";
  return oss.str();
}

std::string to_csv_header() { return "op,latency_ms\n"; }

std::string to_csv_row(double latency_ms, const std::string& op) {
  std::ostringstream oss;
  oss << op << "," << latency_ms << "\n";
  return oss.str();
}

}  // namespace kv::bench
