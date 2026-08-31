cmake_minimum_required(VERSION 3.20)
project(distributed_kv_store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_library(kv_common STATIC
  src/common/protocol.cpp
  src/common/socket_utils.cpp
)
target_include_directories(kv_common PUBLIC src)

add_executable(kvserver
  src/server/main.cpp
  src/server/server.cpp
  src/server/kv_store.cpp
)
target_link_libraries(kvserver PRIVATE kv_common)

add_executable(kvclient
  src/client/main.cpp
)
target_link_libraries(kvclient PRIVATE kv_common)

find_package(Threads REQUIRED)

add_executable(kvbench
  src/benchmark/main.cpp
  src/benchmark/workload.cpp
  src/benchmark/stats.cpp
)
target_link_libraries(kvbench PRIVATE kv_common Threads::Threads)

enable_testing()

add_executable(kv_store_test
  tests/unit/kv_store_test.cpp
  src/server/kv_store.cpp
)
target_include_directories(kv_store_test PRIVATE src)
add_test(NAME kv_store_test COMMAND kv_store_test)

add_executable(protocol_test
  tests/unit/protocol_test.cpp
)
target_link_libraries(protocol_test PRIVATE kv_common)
add_test(NAME protocol_test COMMAND protocol_test)
