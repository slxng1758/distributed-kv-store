#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "router/router.hpp"

namespace {

void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog
      << " --nodes host1:port1,host2:port2,... [--port PORT] [--vnodes N]\n"
      << "  --nodes LIST   comma-separated backend kvserver addresses (required)\n"
      << "  --port PORT    port to listen on for clients (default: 6380)\n"
      << "  --vnodes N     virtual nodes per physical node on the hash ring "
         "(default: 150)\n";
}

std::vector<kv::NodeAddress> parse_nodes(const std::string& list) {
  std::vector<kv::NodeAddress> nodes;
  std::stringstream ss(list);
  std::string item;
  while (std::getline(ss, item, ',')) {
    size_t colon = item.rfind(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("invalid node address (expected host:port): " + item);
    }
    kv::NodeAddress addr;
    addr.host = item.substr(0, colon);
    addr.port = static_cast<uint16_t>(std::atoi(item.substr(colon + 1).c_str()));
    nodes.push_back(addr);
  }
  return nodes;
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 6380;
  size_t vnodes = 150;
  std::string nodes_arg;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
      nodes_arg = argv[++i];
    } else if (std::strcmp(argv[i], "--vnodes") == 0 && i + 1 < argc) {
      vnodes = static_cast<size_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  if (nodes_arg.empty()) {
    std::cerr << "kvrouter: --nodes is required\n";
    print_usage(argv[0]);
    return 1;
  }

  try {
    std::vector<kv::NodeAddress> nodes = parse_nodes(nodes_arg);
    if (nodes.empty()) {
      std::cerr << "kvrouter: --nodes must list at least one node\n";
      return 1;
    }

    kv::Router router(port, nodes, vnodes);
    std::cout << "kvrouter listening on port " << port << ", routing to "
              << nodes.size() << " node(s) via consistent hashing (" << vnodes
              << " vnodes/node)\n";
    router.run();
  } catch (const std::exception& e) {
    std::cerr << "kvrouter: fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
