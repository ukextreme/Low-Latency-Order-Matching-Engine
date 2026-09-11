# Minimum CMake version required
cmake_minimum_required(VERSION 3.16)

# Project name, version, and language
project(matchingengine VERSION 1.0 LANGUAGES CXX)

# Use C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler optimization and warning flags
# -O2: optimize for speed
# -Wall -Wextra: enable most warnings
# -pthread: enable threading support
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pthread")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# ============================================
# Main library (the matching engine core)
# ============================================
# This builds your source files into a library
# that both the main executable and tests can link against.
add_library(engine_lib
    src/order.cpp
    src/orderbook.cpp
    src/orderbook_opt.cpp
)

# Tell the compiler where to find header files
target_include_directories(engine_lib PUBLIC include)

# ============================================
# Main executable
# ============================================
add_executable(engine src/main.cpp)
target_link_libraries(engine engine_lib)

# ============================================
# GoogleTest setup
# ============================================
add_subdirectory(third_party/googletest)

add_executable(engine_tests
    tests/test_orderbook.cpp
    tests/test_orderbook_opt.cpp
    tests/test_spsc_queue.cpp
    tests/test_differential.cpp
)
target_link_libraries(engine_tests engine_lib GTest::gtest_main)
target_include_directories(engine_tests PRIVATE include)

# ============================================
# Google Benchmark setup
# ============================================
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/benchmark)

add_executable(engine_bench
    benchmarks/bench_orderbook.cpp
)
target_link_libraries(engine_bench engine_lib benchmark::benchmark)
target_include_directories(engine_bench PRIVATE include)

add_executable(spsc_bench
    benchmarks/bench_spsc_queue.cpp
)
target_link_libraries(spsc_bench engine_lib benchmark::benchmark)
target_include_directories(spsc_bench PRIVATE include)

# Per-operation latency percentiles (rdtscp), independent of Google Benchmark
add_executable(latency_histogram
    benchmarks/latency_histogram.cpp
)
target_link_libraries(latency_histogram engine_lib)
target_include_directories(latency_histogram PRIVATE include)