#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "server/kv_store.hpp"
#include "server/server.hpp"

namespace {

void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [--port PORT] [--threads N]\n"
      << "  --port PORT     TCP port to listen on (default: 6380)\n"
      << "  --threads N     number of reactor threads (default: 4).\n"
      << "                  --threads 1 reproduces Phase 1's single-\n"
      << "                  threaded behavior on this same binary, which\n"
      << "                  is the cleanest baseline for an apples-to-\n"
      << "                  apples concurrency comparison.\n";
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 6380;
  size_t threads = 4;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      threads = static_cast<size_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  if (threads == 0) {
    std::cerr << "kvserver: --threads must be >= 1\n";
    return 1;
  }

  try {
    kv::KVStore store;
    kv::KvServer server(port, store, threads);
    std::cout << "kvserver listening on port " << port << " (" << threads
              << " reactor thread" << (threads == 1 ? "" : "s") << ")\n";
    server.run();
  } catch (const std::exception& e) {
    std::cerr << "kvserver: fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
