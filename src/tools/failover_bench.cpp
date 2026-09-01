#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "benchmark/workload.hpp"
#include "common/protocol.hpp"
#include "common/socket_utils.hpp"

// kvfailover: generates continuous load against a kvrouter, kills a named
// backend process partway through (via a raw kill() syscall, timed from
// inside this same process to avoid the synchronization slop of
// coordinating the kill through a separate shell script), and measures
// the actual error window around the kill -- Phase 4's headline "time
// until requests are served again" metric, computed from real observed
// request outcomes rather than assumed from the heartbeat interval.

namespace {

struct Config {
  std::string host = "127.0.0.1";
  uint16_t port = 6380;
  int kill_pid = -1;
  double duration_ms = 6000;
  double kill_at_ms = 2000;
  size_t concurrency = 20;
  double get_ratio = 0.7;
  size_t key_space = 1000;
  size_t value_size = 64;
  uint64_t seed = 42;
};

struct Sample {
  double start_ms;
  double end_ms;  // when the outcome (success or failure) became known --
                   // used for before/after-kill classification, since a
                   // request that started just before the kill but whose
                   // in-flight I/O was interrupted by it only reveals its
                   // outcome afterward. Classifying by start_ms instead
                   // would misattribute a real kill-caused failure to
                   // "before the kill" purely because of in-flight overlap.
  bool ok;
  double latency_ms;
};

void worker(size_t thread_index, const Config& cfg,
            std::chrono::steady_clock::time_point start,
            std::vector<Sample>& out_samples) {
  try {
    kv::net::Socket sock = kv::net::connect_to(cfg.host, cfg.port);
    kv::net::set_tcp_nodelay(sock.fd());
    kv::bench::WorkloadGenerator gen(cfg.seed + thread_index, cfg.get_ratio,
                                      cfg.key_space, cfg.value_size);

    while (true) {
      double elapsed_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start)
                               .count();
      if (elapsed_ms >= cfg.duration_ms) break;

      kv::protocol::Request req = gen.next();
      auto t0 = std::chrono::steady_clock::now();
      Sample sample;
      sample.start_ms = std::chrono::duration<double, std::milli>(t0 - start).count();
      try {
        kv::protocol::Response resp = kv::protocol::send_request(sock.fd(), req);
        auto t1 = std::chrono::steady_clock::now();
        sample.end_ms = std::chrono::duration<double, std::milli>(t1 - start).count();
        sample.latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        sample.ok = (resp.type != kv::protocol::ResponseType::Error);
      } catch (const std::exception&) {
        auto t1 = std::chrono::steady_clock::now();
        sample.end_ms = std::chrono::duration<double, std::milli>(t1 - start).count();
        sample.ok = false;
        sample.latency_ms = -1.0;
      }
      out_samples.push_back(sample);
    }
  } catch (const std::exception& e) {
    std::cerr << "kvfailover: worker " << thread_index
              << " failed to connect: " << e.what() << "\n";
  }
}

void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " --kill-pid PID [options]\n"
      << "  --kill-pid PID     PID of the process to SIGKILL mid-run (required)\n"
      << "  --host HOST        router host (default: 127.0.0.1)\n"
      << "  --port PORT        router port (default: 6380)\n"
      << "  --duration-ms N    total run length (default: 6000)\n"
      << "  --kill-at-ms N     when to kill, relative to start (default: 2000)\n"
      << "  --concurrency N    concurrent connections (default: 20)\n"
      << "  --get-ratio R      fraction of GET requests (default: 0.7)\n"
      << "  --key-space N      distinct keys (default: 1000)\n"
      << "  --value-size N     SET value size in bytes (default: 64)\n"
      << "  --seed N           RNG seed (default: 42)\n"
      << "  --output PATH      write a JSON summary to PATH\n";
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  std::string output_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (arg == "--host") cfg.host = next();
    else if (arg == "--port") cfg.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    else if (arg == "--kill-pid") cfg.kill_pid = std::atoi(next().c_str());
    else if (arg == "--duration-ms") cfg.duration_ms = std::stod(next());
    else if (arg == "--kill-at-ms") cfg.kill_at_ms = std::stod(next());
    else if (arg == "--concurrency") cfg.concurrency = std::stoul(next());
    else if (arg == "--get-ratio") cfg.get_ratio = std::stod(next());
    else if (arg == "--key-space") cfg.key_space = std::stoul(next());
    else if (arg == "--value-size") cfg.value_size = std::stoul(next());
    else if (arg == "--seed") cfg.seed = std::stoull(next());
    else if (arg == "--output") output_path = next();
    else if (arg == "--help") { print_usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  if (cfg.kill_pid <= 0) {
    std::cerr << "kvfailover: --kill-pid is required\n";
    print_usage(argv[0]);
    return 1;
  }
  if (cfg.kill_at_ms >= cfg.duration_ms) {
    std::cerr << "kvfailover: --kill-at-ms must be less than --duration-ms\n";
    return 1;
  }

  std::cout << "kvfailover: " << cfg.concurrency << " connections against "
            << cfg.host << ":" << cfg.port << " for " << cfg.duration_ms
            << "ms, killing pid " << cfg.kill_pid << " at t=" << cfg.kill_at_ms
            << "ms\n";

  auto start = std::chrono::steady_clock::now();
  std::vector<std::vector<Sample>> thread_samples(cfg.concurrency);
  std::vector<std::thread> threads;
  threads.reserve(cfg.concurrency);
  for (size_t t = 0; t < cfg.concurrency; ++t) {
    threads.emplace_back(worker, t, std::cref(cfg), start, std::ref(thread_samples[t]));
  }

  std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(cfg.kill_at_ms));
  double kill_ts_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (::kill(cfg.kill_pid, SIGKILL) != 0) {
    std::cerr << "kvfailover: kill(" << cfg.kill_pid
              << ") failed: " << std::strerror(errno) << "\n";
  } else {
    std::cout << "kvfailover: sent SIGKILL to pid " << cfg.kill_pid << " at t="
              << kill_ts_ms << "ms\n";
  }

  for (auto& th : threads) th.join();

  std::vector<Sample> all;
  for (auto& v : thread_samples) {
    for (auto& s : v) all.push_back(s);
  }
  std::sort(all.begin(), all.end(),
            [](const Sample& a, const Sample& b) { return a.end_ms < b.end_ms; });

  size_t errors_before = 0, errors_after = 0;
  size_t requests_before = 0, requests_after = 0;
  double last_error_after_ts = kill_ts_ms;
  bool any_error_after = false;

  // Classified by end_ms (when the outcome became known), not start_ms:
  // a request that started just before the kill but whose in-flight I/O
  // was interrupted by it should count as "after," since that's when its
  // failure actually happened and became observable.
  for (const auto& s : all) {
    if (s.end_ms < kill_ts_ms) {
      ++requests_before;
      if (!s.ok) ++errors_before;
    } else {
      ++requests_after;
      if (!s.ok) {
        ++errors_after;
        any_error_after = true;
        last_error_after_ts = std::max(last_error_after_ts, s.end_ms);
      }
    }
  }
  double recovery_ms = any_error_after ? (last_error_after_ts - kill_ts_ms) : 0.0;

  // "Recovery time" only means what it sounds like if errors actually
  // stopped before the test window ran out. If they were still happening
  // right up to the end, last_error_after_ts is just an artifact of when
  // we stopped measuring, not a real recovery -- e.g. with --replicas 1,
  // a key whose sole owner was killed has no fallback and errors
  // permanently, not for a bounded window. Flag that distinction rather
  // than silently reporting a number that looks like a fast recovery.
  constexpr double kTailWindowMs = 200.0;
  bool errors_at_tail = false;
  for (const auto& s : all) {
    if (s.end_ms >= cfg.duration_ms - kTailWindowMs && !s.ok) {
      errors_at_tail = true;
      break;
    }
  }

  std::cout << "\n=== Failover Results ===\n";
  std::cout << "total requests:      " << all.size() << "\n";
  std::cout << "requests before kill: " << requests_before << " (errors: " << errors_before
            << (errors_before == 0
                    ? ", as expected"
                    : ", UNEXPECTED -- a request outcome was recorded before the kill "
                      "timestamp yet still failed; investigate before trusting this run")
            << ")\n";
  std::cout << "requests after kill:  " << requests_after << " (errors: " << errors_after << ")\n";
  if (!any_error_after) {
    std::cout << "recovery time: 0ms -- no request failed after the kill at all\n";
  } else if (errors_at_tail) {
    std::cout << "recovery time: NOT BOUNDED within this test -- errors were still "
                 "occurring in the final "
              << kTailWindowMs
              << "ms of the run. This looks like a persistent outage (e.g. no "
                 "surviving replica for some keys), not a bounded recovery -- "
                 "rerun with a longer --duration-ms or check --replicas.\n";
  } else {
    std::cout << "recovery time: " << recovery_ms
              << "ms (gap between the kill and the last failed request)\n";
  }

  if (!output_path.empty()) {
    // Coarse 100ms-bucket timeline, mainly useful for eyeballing/plotting
    // the error window around the kill.
    std::map<long long, std::pair<size_t, size_t>> buckets;  // bucket_ms -> (requests, errors)
    for (const auto& s : all) {
      long long bucket = static_cast<long long>(s.end_ms / 100.0) * 100;
      auto& b = buckets[bucket];
      ++b.first;
      if (!s.ok) ++b.second;
    }

    std::ofstream out(output_path);
    if (!out) {
      std::cerr << "kvfailover: could not open " << output_path << " for writing\n";
      return 1;
    }
    out << "{\n"
        << "  \"config\": {\n"
        << "    \"host\": \"" << cfg.host << "\",\n"
        << "    \"port\": " << cfg.port << ",\n"
        << "    \"kill_pid\": " << cfg.kill_pid << ",\n"
        << "    \"duration_ms\": " << cfg.duration_ms << ",\n"
        << "    \"kill_at_ms\": " << cfg.kill_at_ms << ",\n"
        << "    \"concurrency\": " << cfg.concurrency << ",\n"
        << "    \"get_ratio\": " << cfg.get_ratio << ",\n"
        << "    \"key_space\": " << cfg.key_space << ",\n"
        << "    \"value_size\": " << cfg.value_size << ",\n"
        << "    \"seed\": " << cfg.seed << "\n"
        << "  },\n"
        << "  \"kill_timestamp_ms\": " << kill_ts_ms << ",\n"
        << "  \"total_requests\": " << all.size() << ",\n"
        << "  \"requests_before_kill\": " << requests_before << ",\n"
        << "  \"errors_before_kill\": " << errors_before << ",\n"
        << "  \"requests_after_kill\": " << requests_after << ",\n"
        << "  \"errors_after_kill\": " << errors_after << ",\n"
        << "  \"recovery_time_ms\": " << recovery_ms << ",\n"
        << "  \"recovery_bounded\": " << (!any_error_after || !errors_at_tail ? "true" : "false") << ",\n"
        << "  \"timeline_100ms_buckets\": [\n";
    bool first = true;
    for (const auto& [bucket_start, counts] : buckets) {
      if (!first) out << ",\n";
      first = false;
      out << "    {\"bucket_start_ms\": " << bucket_start << ", \"requests\": "
          << counts.first << ", \"errors\": " << counts.second << "}";
    }
    out << "\n  ]\n}\n";
    std::cout << "\nResults written to " << output_path << "\n";
  }

  return 0;
}
