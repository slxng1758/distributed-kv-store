#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/hash_ring.hpp"

// kvhashreport: a standalone, deterministic (no servers required) report
// on Phase 3's two headline sharding metrics -- static key-distribution
// balance across nodes, and what fraction of keys change owner when the
// cluster is resized -- for modulo hashing versus the consistent hash
// ring, side by side.

namespace {

std::vector<std::string> make_keys(size_t n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (size_t i = 0; i < n; ++i) keys.push_back("key:" + std::to_string(i));
  return keys;
}

struct BalanceStats {
  std::map<std::string, size_t> counts;
  double expected = 0.0;
  double max_deviation_pct = 0.0;
};

BalanceStats compute_modulo_balance(const std::vector<std::string>& keys,
                                     size_t num_nodes) {
  BalanceStats stats;
  for (size_t i = 0; i < num_nodes; ++i) stats.counts["node" + std::to_string(i)] = 0;
  for (const auto& k : keys) {
    stats.counts["node" + std::to_string(kv::modulo_hash_node(k, num_nodes))]++;
  }
  stats.expected = static_cast<double>(keys.size()) / static_cast<double>(num_nodes);
  for (const auto& [node, count] : stats.counts) {
    double dev = std::abs(static_cast<double>(count) - stats.expected) / stats.expected * 100.0;
    stats.max_deviation_pct = std::max(stats.max_deviation_pct, dev);
  }
  return stats;
}

BalanceStats compute_ring_balance(const std::vector<std::string>& keys,
                                   const std::vector<std::string>& node_ids,
                                   size_t vnodes) {
  kv::ConsistentHashRing ring(vnodes);
  for (const auto& id : node_ids) ring.add_node(id);

  BalanceStats stats;
  for (const auto& id : node_ids) stats.counts[id] = 0;
  for (const auto& k : keys) stats.counts[ring.get_node(k)]++;
  stats.expected = static_cast<double>(keys.size()) / static_cast<double>(node_ids.size());
  for (const auto& [node, count] : stats.counts) {
    double dev = std::abs(static_cast<double>(count) - stats.expected) / stats.expected * 100.0;
    stats.max_deviation_pct = std::max(stats.max_deviation_pct, dev);
  }
  return stats;
}

// Naive modulo resharding: a key's new owner is its index mod the new
// node count. Node indices aren't stable identities across a resize (node
// "1" under 3 nodes has no relation to node "1" under 4 nodes), so this
// mirrors what actually happens in practice -- almost every key's index
// changes when the divisor changes.
double modulo_remap_fraction(const std::vector<std::string>& keys, size_t before_n,
                              size_t after_n) {
  size_t moved = 0;
  for (const auto& k : keys) {
    if (kv::modulo_hash_node(k, before_n) != kv::modulo_hash_node(k, after_n)) ++moved;
  }
  return static_cast<double>(moved) / static_cast<double>(keys.size());
}

double ring_remap_fraction(const std::vector<std::string>& keys,
                            const std::vector<std::string>& node_ids,
                            const std::string& added_node_id, size_t vnodes) {
  kv::ConsistentHashRing ring(vnodes);
  for (const auto& id : node_ids) ring.add_node(id);

  std::vector<std::string> before(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) before[i] = ring.get_node(keys[i]);

  ring.add_node(added_node_id);

  size_t moved = 0;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (ring.get_node(keys[i]) != before[i]) ++moved;
  }
  return static_cast<double>(moved) / static_cast<double>(keys.size());
}

void print_balance(const std::string& label, const BalanceStats& stats) {
  std::cout << label << " (expected ~" << stats.expected << " keys/node):\n";
  for (const auto& [node, count] : stats.counts) {
    std::cout << "  " << node << ": " << count << "\n";
  }
  std::cout << "  max deviation from ideal: " << stats.max_deviation_pct << "%\n";
}

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [--key-space N] [--vnodes N] [--output PATH]\n"
            << "  --key-space N   number of keys to distribute (default: 1000, "
               "matches kvbench's default keyspace)\n"
            << "  --vnodes N      virtual nodes per physical node (default: 150)\n"
            << "  --output PATH   also write a JSON summary to PATH\n";
}

}  // namespace

int main(int argc, char** argv) {
  size_t key_space = 1000;
  size_t vnodes = 150;
  std::string output_path;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (arg == "--key-space") key_space = std::stoul(next());
    else if (arg == "--vnodes") vnodes = std::stoul(next());
    else if (arg == "--output") output_path = next();
    else if (arg == "--help") { print_usage(argv[0]); return 0; }
    else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  auto keys = make_keys(key_space);
  std::vector<std::string> nodes3 = {"nodeA", "nodeB", "nodeC"};

  std::cout << "=== Static distribution balance across 3 nodes (" << key_space
            << " keys) ===\n\n";
  BalanceStats modulo_stats = compute_modulo_balance(keys, 3);
  print_balance("Modulo hashing", modulo_stats);
  std::cout << "\n";
  BalanceStats ring_stats = compute_ring_balance(keys, nodes3, vnodes);
  print_balance("Consistent hashing (" + std::to_string(vnodes) + " vnodes/node)", ring_stats);

  std::cout << "\n=== Remap fraction when scaling 3 -> 4 nodes (" << key_space
            << " keys) ===\n";
  double modulo_remap = modulo_remap_fraction(keys, 3, 4);
  double ring_remap = ring_remap_fraction(keys, nodes3, "nodeD", vnodes);
  std::cout << "  Modulo hashing:      " << (modulo_remap * 100.0)
            << "% of keys change node\n";
  std::cout << "  Consistent hashing:  " << (ring_remap * 100.0)
            << "% of keys change node (theoretical ideal: ~25%)\n";

  if (!output_path.empty()) {
    std::ofstream out(output_path);
    if (!out) {
      std::cerr << "kvhashreport: could not open " << output_path << " for writing\n";
      return 1;
    }
    out << "{\n"
        << "  \"key_space\": " << key_space << ",\n"
        << "  \"vnodes_per_node\": " << vnodes << ",\n"
        << "  \"static_balance\": {\n"
        << "    \"modulo_max_deviation_pct\": " << modulo_stats.max_deviation_pct << ",\n"
        << "    \"consistent_hash_max_deviation_pct\": " << ring_stats.max_deviation_pct << "\n"
        << "  },\n"
        << "  \"remap_on_scale_3_to_4\": {\n"
        << "    \"modulo_remap_pct\": " << (modulo_remap * 100.0) << ",\n"
        << "    \"consistent_hash_remap_pct\": " << (ring_remap * 100.0) << "\n"
        << "  }\n"
        << "}\n";
    std::cout << "\nResults written to " << output_path << "\n";
  }

  return 0;
}
