#include <iostream>
#include <string>

#include "server/kv_store.hpp"

namespace {

int failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
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

  if (failures > 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "kv_store_test: all tests passed\n";
  return 0;
}
