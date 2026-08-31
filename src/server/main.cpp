#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "server/kv_store.hpp"
#include "server/server.hpp"

namespace {

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [--port PORT]\n"
            << "  --port PORT   TCP port to listen on (default: 6380)\n";
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 6380;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  try {
    kv::KVStore store;
    kv::KvServer server(port, store);
    std::cout << "kvserver listening on port " << port << " (single-threaded, poll() event loop)\n";
    server.run();
  } catch (const std::exception& e) {
    std::cerr << "kvserver: fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
