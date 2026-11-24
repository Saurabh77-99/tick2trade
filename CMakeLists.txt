cmake_minimum_required(VERSION 3.20)
project(hft_stack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / IDE tooling

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(WIN32)
  message(FATAL_ERROR "Build on Linux or macOS. On Windows, use WSL2 (Ubuntu).")
endif()

option(HFT_NATIVE   "Optimize for this machine's CPU (-march=native)" ON)
option(HFT_SANITIZE "Build with AddressSanitizer + UBSan (debugging)" OFF)
option(HFT_TESTS    "Build unit tests (downloads GoogleTest)" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
  if(HFT_NATIVE)
    add_compile_options(-march=native)
  endif()
endif()
if(HFT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# Header-only core library: parser, book, utilities, generator.
add_library(hft INTERFACE)
target_include_directories(hft INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Command-line tool: itch gen | sim | stats | replay | snapshot
add_executable(itch
  src/tools/main.cpp
  src/tools/cmd_sim.cpp
  src/tools/cmd_stats.cpp
  src/tools/cmd_replay.cpp
  src/tools/cmd_snapshot.cpp
  src/tools/cmd_gen.cpp)
target_link_libraries(itch PRIVATE hft)

# Benchmarks
add_executable(bench_book bench/bench_book.cpp)
target_link_libraries(bench_book PRIVATE hft)
add_executable(bench_engine bench/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE hft)

if(HFT_TESTS)
  # Use a system GoogleTest if present (Ubuntu: sudo apt install libgtest-dev),
  # otherwise fetch it.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2
      GIT_SHALLOW    TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(unit_tests
    tests/test_common.cpp
    tests/test_parser.cpp
    tests/test_book.cpp
    tests/test_differential.cpp
    tests/test_engine.cpp)
  target_link_libraries(unit_tests PRIVATE hft GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)
endif()
