cmake_minimum_required(VERSION 3.24)

project(orderbook
  VERSION 0.1.0
  DESCRIPTION "Low latency limit order book, matching engine, ITCH 5.0 replay, and market making simulator"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(OB_BUILD_TESTS "Build the unit, differential, and fuzz test targets" ON)
option(OB_BUILD_BENCH "Build the microbenchmark and latency harness targets" ON)
option(OB_BUILD_TOOLS "Build the synthetic ITCH generator" ON)
option(OB_NATIVE_ARCH "Compile with -march=native" ON)
option(OB_WERROR "Treat warnings as errors" ON)

# ---------------------------------------------------------------------------
# Warning and optimisation flags.
#
# These live on an INTERFACE target rather than in CMAKE_CXX_FLAGS so that the
# FetchContent dependencies are not compiled with -Werror -Wconversion. GoogleTest
# and Google Benchmark do not build clean under those flags and their warnings
# are not ours to fix.
# ---------------------------------------------------------------------------
add_library(ob_flags INTERFACE)

target_compile_options(ob_flags INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
  -Wshadow
  -Wold-style-cast
  -Wnon-virtual-dtor
  -Wdouble-promotion
  -Wformat=2)

if(OB_WERROR)
  target_compile_options(ob_flags INTERFACE -Werror)
endif()

# -fno-omit-frame-pointer is not a debugging nicety here. The latency harness and
# perf stat both need walkable stacks in the optimised build, and omitting the
# frame pointer buys a register that -O3 does not need on this workload.
target_compile_options(ob_flags INTERFACE
  $<$<CONFIG:Release,RelWithDebInfo>:-O3>
  $<$<CONFIG:Release,RelWithDebInfo>:-fno-omit-frame-pointer>)

if(OB_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native OB_HAS_MARCH_NATIVE)
  if(OB_HAS_MARCH_NATIVE)
    target_compile_options(ob_flags INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo>:-march=native>)
  else()
    message(WARNING "-march=native unsupported by this compiler, benchmark numbers will not be comparable")
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library.
#
# Header only at present: everything in include/ob is a data definition or an
# inline template. It becomes a static library once there is a translation unit
# whose code does not belong in a header.
# ---------------------------------------------------------------------------
add_library(ob INTERFACE)

target_include_directories(ob INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(ob INTERFACE ob_flags)

set(OB_PUBLIC_HEADERS
  include/ob/types.hpp
  include/ob/order.hpp
  include/ob/price_level.hpp
  include/ob/events.hpp)

target_sources(ob INTERFACE FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
  FILES ${OB_PUBLIC_HEADERS})

# ---------------------------------------------------------------------------
# Dependencies, vendored through FetchContent so that a clone plus a configure
# is enough to build. Every entry needs a justification in DESIGN.md.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(OB_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)
endif()

if(OB_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.4
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
if(OB_BUILD_TESTS)
  add_executable(ob_tests
    test/test_types.cpp)

  target_link_libraries(ob_tests PRIVATE ob ob_flags GTest::gtest_main)

  gtest_discover_tests(ob_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)
endif()
