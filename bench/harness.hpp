#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

// The measurement discipline, separated from any particular benchmark.
//
// Phase 4's deliverable is the methodology rather than the numbers, and this
// header is where the methodology lives. Everything here exists to answer one
// question about a published figure: under what conditions was it taken, and is
// the reader entitled to believe it.
//
// The rule this follows throughout: **fail loudly, never silently**. A harness
// that quietly falls back to a worse clock, or quietly fails to pin a thread, and
// then prints a confident number is worse than one that refuses to run, because
// the number outlives the caveat. Every degraded condition here is both recorded
// in the reported environment and printed as a warning.

namespace ob::bench {

// ---------------------------------------------------------------------------
// Timing.
// ---------------------------------------------------------------------------

enum class ClockSource : std::uint8_t {
  invariant_tsc,  // rdtscp, verified invariant through CPUID
  steady_clock,   // the documented fallback
};

struct TimerInfo {
  ClockSource source = ClockSource::steady_clock;

  // Only meaningful when source is invariant_tsc. Calibrated at startup against
  // steady_clock.
  double ticks_per_ns = 0.0;

  bool cpuid_available = false;
  bool tsc_invariant = false;

  // Why the fallback was taken, empty when the TSC is in use.
  std::string fallback_reason;
};

// Calibrated once on first call. Not thread safe by design: the harness is single
// threaded and a mutex here would be measured.
const TimerInfo& timer_info();

// Nanoseconds represented by a tick count from the active clock source.
[[nodiscard]] double ticks_to_ns(std::uint64_t ticks);

// ---------------------------------------------------------------------------
// Core pinning.
//
// sched_setaffinity on POSIX, SetThreadAffinityMask on Windows. Windows has no
// equivalent of isolcpus, so pinning there stops the thread migrating between
// performance and efficiency cores but does not stop other work being scheduled
// onto the same core. That distinction is reported rather than glossed over.
// ---------------------------------------------------------------------------

enum class PinStatus : std::uint8_t {
  pinned,
  not_requested,
  unsupported,
  failed,
};

struct PinInfo {
  PinStatus status = PinStatus::not_requested;
  int cpu = -1;
  std::string detail;
};

PinInfo pin_to_cpu(int cpu);

// ---------------------------------------------------------------------------
// Frequency scaling.
//
// Readable on Linux through sysfs. Not readable on Windows through any interface
// this project is willing to depend on, so it reports unknown there, which is the
// honest answer and is treated as a warning rather than as a pass.
// ---------------------------------------------------------------------------

enum class TurboStatus : std::uint8_t {
  active,
  disabled,
  unknown,
};

struct TurboInfo {
  TurboStatus status = TurboStatus::unknown;
  std::string detail;
};

TurboInfo turbo_state();

// ---------------------------------------------------------------------------
// Environment capture. Printed with every result, because a latency figure
// without its machine is not a result.
// ---------------------------------------------------------------------------

struct Environment {
  std::string cpu_brand;
  unsigned hardware_threads = 0;
  std::string os;
  std::string compiler;
  std::string build_type;
  std::string build_flags;
};

Environment capture_environment();

// Writes the full block: environment, clock source and calibration, pinning
// state, turbo state, and any warnings the above produced. Returns the number of
// warnings, so a caller can decide to be noisier about a degraded run.
int print_environment(std::ostream& out,
                      const Environment& environment,
                      const TimerInfo& timer,
                      const PinInfo& pin,
                      const TurboInfo& turbo);

}  // namespace ob::bench

// ---------------------------------------------------------------------------
// The timestamp reads themselves, inline in the header so that a call does not
// appear inside the measured region.
//
// rdtscp rather than rdtsc, because rdtscp does not begin until every prior
// instruction has retired, so it cannot float above the work being measured. The
// remaining hazard is the other direction, later instructions being hoisted above
// the read, which lfence closes. Start fences after the read and end fences
// before it, so the measured region cannot leak in either direction.
// ---------------------------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#define OB_BENCH_HAS_TSC 1
#else
#define OB_BENCH_HAS_TSC 0
#endif

#if OB_BENCH_HAS_TSC
#include <x86intrin.h>
#endif

#include <chrono>

namespace ob::bench {

[[nodiscard]] inline std::uint64_t tick_start() noexcept {
#if OB_BENCH_HAS_TSC
  if (timer_info().source == ClockSource::invariant_tsc) {
    unsigned aux = 0;
    const std::uint64_t stamp = __rdtscp(&aux);
    _mm_lfence();
    return stamp;
  }
#endif
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

[[nodiscard]] inline std::uint64_t tick_end() noexcept {
#if OB_BENCH_HAS_TSC
  if (timer_info().source == ClockSource::invariant_tsc) {
    _mm_lfence();
    unsigned aux = 0;
    return __rdtscp(&aux);
  }
#endif
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace ob::bench
