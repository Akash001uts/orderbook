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
option(OB_BUILD_FUZZERS "Build the libFuzzer differential target, requires Clang" OFF)
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

include(CheckCXXCompilerFlag)

if(OB_NATIVE_ARCH)
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
# The hot path lives in headers because it must inline: Book is a template over
# the band size and its operations are a handful of instructions each, so a call
# across a translation unit boundary would cost more than the work. The only
# translation unit is the overflow cold path, which is deliberately out of line
# so that <map> cannot be reached from any header the hot path includes.
# ---------------------------------------------------------------------------
add_library(ob STATIC
  src/book.cpp)

target_include_directories(ob PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(ob PUBLIC ob_flags)

set(OB_PUBLIC_HEADERS
  include/ob/types.hpp
  include/ob/order.hpp
  include/ob/order_pool.hpp
  include/ob/order_map.hpp
  include/ob/price_level.hpp
  include/ob/level_bitmap.hpp
  include/ob/book.hpp
  include/ob/events.hpp)

target_sources(ob PUBLIC FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
  FILES ${OB_PUBLIC_HEADERS})

# ---------------------------------------------------------------------------
# ITCH 5.0 parser, replay driver, and synthetic generator.
#
# A separate target from ob because it depends on the platform: mapped_file.cpp is
# the only file in the project that includes windows.h or sys/mman.h, and keeping
# that behind its own library means nothing in the core book can accidentally
# acquire a platform dependency.
# ---------------------------------------------------------------------------
add_library(ob_itch STATIC
  itch/src/parser.cpp
  itch/src/mapped_file.cpp
  itch/src/synthetic.cpp)

target_include_directories(ob_itch PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/itch/include>)

target_link_libraries(ob_itch PUBLIC ob ob_flags)

target_sources(ob_itch PUBLIC FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/itch/include
  FILES
    itch/include/itch/messages.hpp
    itch/include/itch/parser.hpp
    itch/include/itch/mapped_file.hpp
    itch/include/itch/replay.hpp
    itch/include/itch/synthetic.hpp)

# ---------------------------------------------------------------------------
# Strategy layer.
#
# Header only, like the engine, and for the same reason: everything here is a
# template over the band size or small enough to inline, and a translation unit
# boundary in the middle of the fill path would hide the thing the benchmarks
# measure. It depends on both ob and ob_itch because a backtest is the two halves
# meeting: a venue book reconstructed from ITCH, and hypothetical orders matched
# against it by the engine.
# ---------------------------------------------------------------------------
add_library(ob_strategy INTERFACE)

target_include_directories(ob_strategy INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/strategy/include)
target_link_libraries(ob_strategy INTERFACE ob ob_itch)

target_sources(ob_strategy INTERFACE FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/strategy/include
  FILES
    strategy/include/strategy/queue_position.hpp
    strategy/include/strategy/pnl.hpp
    strategy/include/strategy/market_maker.hpp
    strategy/include/strategy/backtest.hpp)

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
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  # Google Benchmark v1.9.4 compiles its own sources with -pedantic-errors, added
  # unconditionally rather than behind BENCHMARK_ENABLE_WERROR, and its public
  # header tests __COUNTER__ in a preprocessor conditional. Clang 20 and newer
  # report __COUNTER__ as a C2y extension, so the two combine to break the
  # dependency's own build on a new Clang. Disabling that one diagnostic on the
  # dependency's targets is the least invasive fix available and it does not touch
  # the flags applied to this project's code.
  check_cxx_compiler_flag(-Wc2y-extensions OB_HAS_WC2Y_EXTENSIONS)
  if(OB_HAS_WC2Y_EXTENSIONS)
    foreach(bm_target benchmark benchmark_main)
      if(TARGET ${bm_target})
        target_compile_options(${bm_target} PRIVATE -Wno-c2y-extensions)
      endif()
    endforeach()
  endif()

  # HdrHistogram, for the latency harness only.
  #
  # Justification, per the rule above that every dependency needs one. Latency in
  # this domain is a tail question, and the tail is where a naive histogram is
  # worst: fixed width buckets either lose all resolution at the top of the range
  # or need absurd memory to keep it. HdrHistogram records across a wide dynamic
  # range at a constant relative precision, which is exactly the shape of the
  # problem, and it is the reference implementation of that idea rather than
  # something written here and unvalidated.
  #
  # Deliberately not fetched until now. An unused dependency is still a dependency
  # someone has to build, so it arrives with the harness that needs it.
  #
  # The log writer is disabled because it is the only part that needs zlib, and
  # nothing here writes histogram logs. Programs and the shared library are off for
  # the same reason: this build needs the static core and nothing else.
  #
  # GIT_SUBMODULES "" because the only submodule it declares is its own vendored
  # copy of Google Benchmark, used by its test suite. This build does not run that
  # suite, and cloning a second Google Benchmark on every fresh CI configure is
  # pure cost.
  FetchContent_Declare(hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG 0.11.8
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SYSTEM
    EXCLUDE_FROM_ALL)
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
  set(HDR_LOG_REQUIRED OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(hdr_histogram)
endif()

# Force the fetched dependencies' interface includes to be treated as system
# headers by anything that consumes them.
#
# SYSTEM on FetchContent_Declare marks the subdirectory, but it does not reliably
# reach the interface include directories that a consuming target inherits, so
# warnings from a dependency's header still surface in our translation unit. Google
# Benchmark is the concrete case: benchmark.h uses __COUNTER__, which -Wpedantic
# reports as a non-standard extension, and with -Werror that failed the bench
# target on Clang. The warning is legitimate and it is not ours to fix.
#
# Re-declaring the same directories as SYSTEM is the portable form of this. The
# SYSTEM target property would be tidier but it needs CMake 3.25, and this project
# targets 3.24.
function(ob_mark_dependency_system target_name)
  if(NOT TARGET ${target_name})
    return()
  endif()
  get_target_property(dep_includes ${target_name} INTERFACE_INCLUDE_DIRECTORIES)
  if(dep_includes)
    target_include_directories(${target_name} SYSTEM INTERFACE ${dep_includes})
  endif()
endfunction()

foreach(dep_target gtest gtest_main benchmark benchmark_main hdr_histogram_static)
  ob_mark_dependency_system(${dep_target})
endforeach()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
if(OB_BUILD_TESTS)
  add_executable(ob_tests
    test/alloc_counter.cpp
    test/test_types.cpp
    test/test_book.cpp
    test/test_engine.cpp
    test/test_differential.cpp
    test/test_itch.cpp
    test/test_strategy.cpp)

  target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(ob_tests PRIVATE ob ob_itch ob_strategy ob_flags GTest::gtest_main)

  # The differential test writes shrunk reproducers into the source tree, where
  # they are committed and replayed on every later run. It therefore needs to know
  # where the source tree is, which a build directory cannot tell it.
  target_compile_definitions(ob_tests PRIVATE
    OB_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test")

  # The differential test dominates the suite's runtime, so it gets a longer
  # timeout and its own label. The nightly ten million command run needs it.
  gtest_discover_tests(ob_tests
    DISCOVERY_MODE PRE_TEST
    TEST_SUFFIX ""
    PROPERTIES LABELS unit TIMEOUT 1800)
endif()

# ---------------------------------------------------------------------------
# Fuzzing.
#
# Off by default and Clang only, because -fsanitize=fuzzer is a Clang feature and
# because a fuzz target is not something a plain build should have to link. It
# shares its comparison logic with the differential test through
# test/differential_harness.hpp, so a bug the fuzzer finds reproduces under the
# test rather than only under the fuzzer.
# ---------------------------------------------------------------------------
if(OB_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "OB_BUILD_FUZZERS requires Clang, found ${CMAKE_CXX_COMPILER_ID}")
  endif()

  add_executable(ob_fuzz_differential
    test/fuzz_differential.cpp)

  target_include_directories(ob_fuzz_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(ob_fuzz_differential PRIVATE ob ob_flags)
  target_compile_options(ob_fuzz_differential PRIVATE -fsanitize=fuzzer,address)
  target_link_options(ob_fuzz_differential PRIVATE -fsanitize=fuzzer,address)
endif()

# ---------------------------------------------------------------------------
# Benchmarks.
# ---------------------------------------------------------------------------
if(OB_BUILD_BENCH)
  # The BENCHMARK registration macro expands __COUNTER__, which -Wpedantic reports
  # as a non-standard extension. Marking the dependency's includes as system does
  # not help: the diagnostic fires because the macro is expanded in our translation
  # unit, not because the header was parsed. So the single diagnostic is disabled
  # for benchmark targets only, and every other warning stays an error there.
  #
  # The flag is probed in its positive form because compilers accept an unknown
  # -Wno-anything silently, which would make a check of the negative form always
  # succeed and then fail the build on -Wunknown-warning-option.
  add_library(ob_bench_flags INTERFACE)
  if(OB_HAS_WC2Y_EXTENSIONS)
    target_compile_options(ob_bench_flags INTERFACE -Wno-c2y-extensions)
  endif()

  add_executable(ob_micro_bench
    bench/micro_bench.cpp)

  target_link_libraries(ob_micro_bench PRIVATE ob ob_flags ob_bench_flags benchmark::benchmark)

  add_executable(ob_replay_bench
    bench/replay_bench.cpp)

  target_link_libraries(ob_replay_bench
    PRIVATE ob ob_itch ob_flags ob_bench_flags benchmark::benchmark)

  # The comparative baseline reaches into test/ for two things that already exist
  # and should not be duplicated: reference_book.hpp, which is the std::map book
  # the differential test uses as its oracle, and alloc_counter.cpp, which
  # replaces global operator new so the allocation difference between the two
  # implementations can be counted exactly rather than asserted.
  #
  # Reusing the oracle is the point rather than a shortcut. A baseline written
  # fresh for the benchmark would be a strawman nobody had checked; this one is
  # the implementation whose agreement with the engine is what the correctness
  # argument rests on.
  add_executable(ob_baseline_bench
    bench/baseline_bench.cpp
    test/alloc_counter.cpp)

  target_include_directories(ob_baseline_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(ob_baseline_bench PRIVATE ob ob_flags ob_bench_flags benchmark::benchmark)

  # The measurement discipline: TSC timing with a verified invariance bit, core
  # pinning, turbo detection, and environment capture. Shared by anything that
  # publishes a number, and separate from any one benchmark because the
  # methodology is the phase's deliverable rather than a detail of one target.
  add_library(ob_bench_harness STATIC
    bench/harness.cpp)

  target_include_directories(ob_bench_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bench)
  target_link_libraries(ob_bench_harness PRIVATE ob_flags)

  # The environment block prints the flags a result was built with, which means
  # the build has to hand them to the code. Doing it any other way invites the
  # printed flags and the real ones to drift apart.
  string(TOUPPER "${CMAKE_BUILD_TYPE}" ob_build_type_upper)
  target_compile_definitions(ob_bench_harness PRIVATE
    OB_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    OB_BUILD_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${ob_build_type_upper}}")

  # The latency harness is a program rather than a Google Benchmark target. It
  # replays a real capture once per run and reports a distribution, which is not
  # what a repetition-driven microbenchmark framework is for.
  add_executable(ob_latency_bench
    bench/latency_bench.cpp)

  target_link_libraries(ob_latency_bench
    PRIVATE ob ob_itch ob_flags ob_bench_harness hdr_histogram_static)
endif()

# ---------------------------------------------------------------------------
# Tools.
# ---------------------------------------------------------------------------
if(OB_BUILD_TOOLS)
  add_executable(itch_gen
    tools/itch_gen.cpp)

  target_link_libraries(itch_gen PRIVATE ob_itch ob_flags)

  add_executable(itch_replay
    tools/itch_replay.cpp)

  target_link_libraries(itch_replay PRIVATE ob_itch ob_flags)

  add_executable(ob_strategy_backtest
    tools/strategy_backtest.cpp)

  target_link_libraries(ob_strategy_backtest PRIVATE ob_strategy ob_flags)
endif()
