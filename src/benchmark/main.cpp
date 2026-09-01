#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "benchmark/stats.hpp"
#include "benchmark/workload.hpp"
#include "common/protocol.hpp"
#include "common/socket_utils.hpp"

// kvbench: with no arguments, runs the standard reproducible workload
// (10,000 requests / 50 concurrent persistent connections / 70:30 GET:SET)
// so results stay comparable, unchanged, across every later phase.

namespace {

struct ThreadResult {
  std::vector<double> get_latencies_ms;
  std::vector<double> set_latencies_ms;
  size_t errors = 0;
};

void worker(size_t thread_index, size_t num_requests,
            const kv::bench::RunConfig& cfg, ThreadResult& result) {
  try {
    kv::net::Socket sock = kv::net::connect_to(cfg.host, cfg.port);
    kv::net::set_tcp_nodelay(sock.fd());
    kv::bench::WorkloadGenerator gen(cfg.seed + thread_index, cfg.get_ratio,
                                      cfg.key_space, cfg.value_size);

    for (size_t i = 0; i < num_requests; ++i) {
      kv::protocol::Request req = gen.next();
      auto t0 = std::chrono::steady_clock::now();
      try {
        // Closed-loop, non-pipelined measurement, matching realistic
        // client behavior (one request in flight at a time).
        kv::protocol::Response resp = kv::protocol::send_request(sock.fd(), req);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (resp.type == kv::protocol::ResponseType::Error) {
          ++result.errors;
        } else if (req.command == kv::protocol::Command::Get) {
          result.get_latencies_ms.push_back(ms);
        } else {
          result.set_latencies_ms.push_back(ms);
        }
      } catch (const std::exception&) {
        ++result.errors;
      }
    }
  } catch (const std::exception& e) {
    // Couldn't even connect; count every request assigned to this worker
    // as failed rather than silently under-reporting.
    result.errors += num_requests;
    std::cerr << "kvbench: worker " << thread_index << " failed: " << e.what()
              << "\n";
  }
}

std::string get_git_rev() {
  std::array<char, 128> buf{};
  std::string result;
  FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (!pipe) return "unknown";
  while (fgets(buf.data(), buf.size(), pipe) != nullptr) result += buf.data();
  pclose(pipe);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result.empty() ? "unknown" : result;
}

std::string timestamp_run_id() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y%m%d-%H%M%S");
  return oss.str();
}

void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "  --host HOST          server host (default: 127.0.0.1)\n"
      << "  --port PORT          server port (default: 6380)\n"
      << "  --requests N         total requests (default: 10000)\n"
      << "  --concurrency N      concurrent connections (default: 50)\n"
      << "  --get-ratio R        fraction of GET requests, 0..1 (default: 0.7)\n"
      << "  --key-space N        number of distinct keys (default: 1000)\n"
      << "  --value-size N       SET value size in bytes (default: 64)\n"
      << "  --seed N             RNG seed (default: 42)\n"
      << "  --output PATH        results JSON path (default: results/<timestamp>.json)\n"
      << "  --raw-output         also write a CSV of every raw latency sample\n";
}

}  // namespace

int main(int argc, char** argv) {
  kv::bench::RunConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 6380;
  cfg.requests = 10000;
  cfg.concurrency = 50;
  cfg.get_ratio = 0.7;
  cfg.key_space = 1000;
  cfg.value_size = 64;
  cfg.seed = 42;

  std::string output_path;
  bool raw_output = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (arg == "--host") cfg.host = next();
    else if (arg == "--port") cfg.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    else if (arg == "--requests") cfg.requests = std::stoul(next());
    else if (arg == "--concurrency") cfg.concurrency = std::stoul(next());
    else if (arg == "--get-ratio") cfg.get_ratio = std::stod(next());
    else if (arg == "--key-space") cfg.key_space = std::stoul(next());
    else if (arg == "--value-size") cfg.value_size = std::stoul(next());
    else if (arg == "--seed") cfg.seed = std::stoull(next());
    else if (arg == "--output") output_path = next();
    else if (arg == "--raw-output") raw_output = true;
    else if (arg == "--help") { print_usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  if (cfg.requests == 0 || cfg.concurrency == 0) {
    std::cerr << "kvbench: --requests and --concurrency must be > 0\n";
    return 1;
  }

  cfg.git_rev = get_git_rev();
  std::string run_id = timestamp_run_id();
  if (output_path.empty()) output_path = "results/" + run_id + ".json";

  size_t base = cfg.requests / cfg.concurrency;
  size_t remainder = cfg.requests % cfg.concurrency;

  std::vector<ThreadResult> thread_results(cfg.concurrency);
  std::vector<std::thread> threads;
  threads.reserve(cfg.concurrency);

  std::cout << "kvbench: running " << cfg.requests << " requests across "
            << cfg.concurrency << " connections against " << cfg.host << ":"
            << cfg.port << "...\n";

  auto wall_start = std::chrono::steady_clock::now();
  for (size_t t = 0; t < cfg.concurrency; ++t) {
    size_t n = base + (t < remainder ? 1 : 0);
    threads.emplace_back(worker, t, n, std::cref(cfg), std::ref(thread_results[t]));
  }
  for (auto& th : threads) th.join();
  auto wall_end = std::chrono::steady_clock::now();

  std::vector<double> all_latencies, get_latencies, set_latencies;
  size_t total_errors = 0;
  for (auto& tr : thread_results) {
    total_errors += tr.errors;
    for (double v : tr.get_latencies_ms) {
      get_latencies.push_back(v);
      all_latencies.push_back(v);
    }
    for (double v : tr.set_latencies_ms) {
      set_latencies.push_back(v);
      all_latencies.push_back(v);
    }
  }

  kv::bench::RunResult result;
  result.config = cfg;
  result.wall_clock_seconds =
      std::chrono::duration<double>(wall_end - wall_start).count();
  result.throughput_rps = result.wall_clock_seconds > 0
                               ? static_cast<double>(all_latencies.size()) /
                                     result.wall_clock_seconds
                               : 0.0;
  result.overall = kv::bench::compute_percentiles(all_latencies);
  result.get_stats = kv::bench::compute_percentiles(get_latencies);
  result.set_stats = kv::bench::compute_percentiles(set_latencies);
  result.error_count = total_errors;

  kv::bench::print_summary(result);

  std::ofstream json_out(output_path);
  if (!json_out) {
    std::cerr << "kvbench: could not open " << output_path << " for writing\n";
    return 1;
  }
  json_out << kv::bench::to_json(result);
  json_out.close();
  std::cout << "Results written to " << output_path << "\n";

  if (raw_output) {
    std::string csv_path = output_path;
    size_t dot = csv_path.rfind(".json");
    if (dot != std::string::npos) csv_path.resize(dot);
    csv_path += ".csv";

    std::ofstream csv_out(csv_path);
    if (csv_out) {
      csv_out << kv::bench::to_csv_header();
      for (auto& tr : thread_results) {
        for (double v : tr.get_latencies_ms) csv_out << kv::bench::to_csv_row(v, "GET");
        for (double v : tr.set_latencies_ms) csv_out << kv::bench::to_csv_row(v, "SET");
      }
      std::cout << "Raw samples written to " << csv_path << "\n";
    }
  }

  return total_errors > 0 ? 2 : 0;
}
