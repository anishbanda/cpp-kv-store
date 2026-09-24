cmake_minimum_required(VERSION 3.20)

project(
  cpp_kv_store
  VERSION 0.1.0
  DESCRIPTION "A high-performance in-memory key-value server in modern C++"
  LANGUAGES CXX
)

option(KVSTORE_BUILD_TESTS "Build the test suite" ON)
option(KVSTORE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(KVSTORE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(KVSTORE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectWarnings)
include(Sanitizers)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(kvstore_core STATIC
  src/version.cpp
  src/storage/clock.cpp
  src/storage/store.cpp
  src/storage/ttl_sweeper.cpp
  src/protocol/parser.cpp
  src/protocol/serializer.cpp
  src/net/file_descriptor.cpp
  src/net/bounded_buffer.cpp
  src/net/connection.cpp
  src/net/connection_registry.cpp
  src/net/io.cpp
  src/net/listener.cpp
  src/net/event_loop.cpp
  src/net/response_sequencer.cpp
  src/command/dispatcher.cpp
  src/worker/worker_pool.cpp
)

target_include_directories(kvstore_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(kvstore_core PUBLIC Threads::Threads)

kvstore_set_project_warnings(kvstore_core)
kvstore_enable_sanitizers(kvstore_core)

add_executable(kvstore_server src/main.cpp)
target_link_libraries(kvstore_server PRIVATE kvstore_core)
kvstore_set_project_warnings(kvstore_server)
kvstore_enable_sanitizers(kvstore_server)

if(KVSTORE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  find_package(GTest REQUIRED)

  add_executable(kvstore_tests
    tests/smoke_test.cpp
    tests/storage/store_test.cpp
    tests/storage/ttl_sweeper_test.cpp
    tests/protocol/parser_test.cpp
    tests/protocol/serializer_test.cpp
    tests/net/file_descriptor_test.cpp
    tests/net/bounded_buffer_test.cpp
    tests/net/io_test.cpp
    tests/net/connection_registry_test.cpp
    tests/net/listener_test.cpp
    tests/net/event_loop_test.cpp
    tests/net/response_sequencer_test.cpp
    tests/worker/bounded_queue_test.cpp
    tests/worker/worker_pool_test.cpp
    tests/command/dispatcher_test.cpp
  )
  target_link_libraries(kvstore_tests PRIVATE kvstore_core GTest::gtest_main)
  kvstore_set_project_warnings(kvstore_tests)
  kvstore_enable_sanitizers(kvstore_tests)

  include(GoogleTest)
  gtest_discover_tests(kvstore_tests)
endif()

