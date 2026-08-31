cmake_minimum_required(VERSION 3.20)
project(distributed_kv_store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Build with `cmake -S . -B build -DENABLE_TSAN=ON` to verify the Phase 2
# concurrency model (worker reactors + shared KVStore) is race-free.
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(kv_common STATIC
  src/common/protocol.cpp
  src/common/socket_utils.cpp
  src/common/hash_ring.cpp
)
target_include_directories(kv_common PUBLIC src)

add_executable(kvserver
  src/server/main.cpp
  src/server/server.cpp
  src/server/worker_reactor.cpp
  src/server/kv_store.cpp
)
target_link_libraries(kvserver PRIVATE kv_common Threads::Threads)

add_executable(kvclient
  src/client/main.cpp
)
target_link_libraries(kvclient PRIVATE kv_common)

add_executable(kvbench
  src/benchmark/main.cpp
  src/benchmark/workload.cpp
  src/benchmark/stats.cpp
)
target_link_libraries(kvbench PRIVATE kv_common Threads::Threads)

add_executable(kvrouter
  src/router/main.cpp
  src/router/router.cpp
)
target_link_libraries(kvrouter PRIVATE kv_common Threads::Threads)

add_executable(kvhashreport
  src/tools/hash_report.cpp
)
target_link_libraries(kvhashreport PRIVATE kv_common)

enable_testing()

add_executable(kv_store_test
  tests/unit/kv_store_test.cpp
  src/server/kv_store.cpp
)
target_include_directories(kv_store_test PRIVATE src)
target_link_libraries(kv_store_test PRIVATE Threads::Threads)
add_test(NAME kv_store_test COMMAND kv_store_test)

add_executable(protocol_test
  tests/unit/protocol_test.cpp
)
target_link_libraries(protocol_test PRIVATE kv_common)
add_test(NAME protocol_test COMMAND protocol_test)

add_executable(hash_ring_test
  tests/unit/hash_ring_test.cpp
)
target_link_libraries(hash_ring_test PRIVATE kv_common)
add_test(NAME hash_ring_test COMMAND hash_ring_test)
