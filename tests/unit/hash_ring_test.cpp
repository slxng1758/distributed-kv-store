#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/hash_ring.hpp"

namespace {

int failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

std::vector<std::string> make_keys(size_t n) {
  std::vector<std::string> keys;
  keys.reserve(n);
  for (size_t i = 0; i < n; ++i) keys.push_back("key:" + std::to_string(i));
  return keys;
}

void test_determinism() {
  kv::ConsistentHashRing ring(150);
  ring.add_node("nodeA");
  ring.add_node("nodeB");
  ring.add_node("nodeC");

  for (const auto& k : make_keys(500)) {
    std::string first = ring.get_node(k);
    for (int i = 0; i < 5; ++i) {
      check(ring.get_node(k) == first,
            "repeated lookups of the same key on an unchanged ring must agree");
    }
  }
}

void test_all_nodes_used() {
  kv::ConsistentHashRing ring(150);
  ring.add_node("nodeA");
  ring.add_node("nodeB");
  ring.add_node("nodeC");

  std::unordered_map<std::string, int> counts;
  for (const auto& k : make_keys(3000)) counts[ring.get_node(k)]++;

  check(counts.size() == 3, "all three nodes should receive at least one key across 3000 keys");
}

void test_distribution_balance() {
  kv::ConsistentHashRing ring(150);
  ring.add_node("nodeA");
  ring.add_node("nodeB");
  ring.add_node("nodeC");

  auto keys = make_keys(10000);
  std::unordered_map<std::string, int> counts;
  for (const auto& k : keys) counts[ring.get_node(k)]++;

  double expected = static_cast<double>(keys.size()) / 3.0;
  for (const auto& [node, count] : counts) {
    double deviation = std::abs(count - expected) / expected;
    check(deviation < 0.15,
          "node " + node + " got " + std::to_string(count) + " of " +
              std::to_string(keys.size()) + " keys (expected ~" +
              std::to_string(static_cast<int>(expected)) +
              "); deviation should stay under 15% with 150 vnodes/node, got " +
              std::to_string(deviation * 100) + "%");
  }
}

// The property that makes consistent hashing worth its extra complexity
// over modulo hashing: removing one node must only move keys that were
// actually owned by that node. Every other key's owner stays put, because
// it never depended on the total node count.
void test_removing_a_node_only_moves_its_own_keys() {
  kv::ConsistentHashRing ring(150);
  ring.add_node("nodeA");
  ring.add_node("nodeB");
  ring.add_node("nodeC");

  auto keys = make_keys(2000);
  std::unordered_map<std::string, std::string> before;
  for (const auto& k : keys) before[k] = ring.get_node(k);

  ring.remove_node("nodeB");

  int unexpected_moves = 0;
  for (const auto& k : keys) {
    std::string prev = before[k];
    std::string now = ring.get_node(k);
    if (prev != "nodeB" && prev != now) ++unexpected_moves;
  }

  check(unexpected_moves == 0,
        "removing a node must not move keys that weren't on that node -- this "
        "is the whole point of consistent hashing over modulo hashing");
}

void test_adding_a_node_moves_a_minority_of_keys() {
  kv::ConsistentHashRing ring(150);
  ring.add_node("nodeA");
  ring.add_node("nodeB");
  ring.add_node("nodeC");

  auto keys = make_keys(10000);
  std::unordered_map<std::string, std::string> before;
  for (const auto& k : keys) before[k] = ring.get_node(k);

  ring.add_node("nodeD");

  int moved = 0;
  for (const auto& k : keys) {
    if (ring.get_node(k) != before[k]) ++moved;
  }
  double moved_fraction = static_cast<double>(moved) / static_cast<double>(keys.size());

  check(moved_fraction < 0.40,
        "adding a 4th node should remap roughly 1/4 of keys (theoretical ideal "
        "~25%), not most of them -- got " + std::to_string(moved_fraction * 100) + "%");
}

void test_modulo_hash_range() {
  for (size_t i = 0; i < 1000; ++i) {
    size_t n = kv::modulo_hash_node("key:" + std::to_string(i), 3);
    check(n < 3, "modulo_hash_node result must be within [0, num_nodes)");
  }
}

}  // namespace

int main() {
  test_determinism();
  test_all_nodes_used();
  test_distribution_balance();
  test_removing_a_node_only_moves_its_own_keys();
  test_adding_a_node_moves_a_minority_of_keys();
  test_modulo_hash_range();

  if (failures > 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "hash_ring_test: all tests passed\n";
  return 0;
}
