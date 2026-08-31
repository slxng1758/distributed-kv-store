#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "server/kv_store.hpp"

namespace {

int failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

// Phase 2: many threads concurrently SET distinct keys and immediately GET
// them back. This checks what the shared_mutex actually guarantees --
// individual operations are race-free -- without assuming atomicity across
// separate GET+SET calls, which KVStore does NOT provide (see the
// read-modify-write caveat documented in kv_store.hpp). Build with
// -DENABLE_TSAN=ON to have ThreadSanitizer confirm there's no data race
// underneath these results.
void test_concurrent_distinct_keys() {
  kv::KVStore store;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 2000;

  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&store, &mismatches, t] {
      for (int i = 0; i < kPerThread; ++i) {
        std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
        std::string value = "v" + std::to_string(t) + "-" + std::to_string(i);
        store.set(key, value);

        std::string out;
        if (!store.get(key, out) || out != value) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  check(mismatches.load() == 0,
        "every concurrent SET followed by GET on its own key should read "
        "back exactly what was written");
  check(store.size() == static_cast<size_t>(kThreads * kPerThread),
        "store should contain exactly one entry per distinct key written, "
        "with none lost or duplicated under concurrent access");
}

}  // namespace

int main() {
  kv::KVStore store;
  std::string out;

  check(!store.get("missing", out), "get on empty store should return false");
  check(store.size() == 0, "size should start at 0");

  store.set("foo", "bar");
  check(store.get("foo", out) && out == "bar",
        "get after set should return the value");
  check(store.size() == 1, "size should be 1 after one set");

  store.set("foo", "baz");
  check(store.get("foo", out) && out == "baz",
        "set should overwrite an existing value");
  check(store.size() == 1, "overwriting an existing key should not change size");

  check(store.remove("foo"), "remove on an existing key should return true");
  check(!store.get("foo", out), "get after remove should return false");
  check(store.size() == 0, "size should be 0 after removing the only key");

  check(!store.remove("foo"), "remove on a missing key should return false");

  test_concurrent_distinct_keys();

  if (failures > 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "kv_store_test: all tests passed\n";
  return 0;
}
