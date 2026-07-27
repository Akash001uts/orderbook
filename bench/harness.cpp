#include "harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#if defined(__GNUC__) && OB_BENCH_HAS_TSC
#include <cpuid.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

namespace ob::bench {
namespace {

// ---------------------------------------------------------------------------
// CPUID.
// ---------------------------------------------------------------------------

struct CpuidRegisters {
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
};

[[nodiscard]] bool cpuid(unsigned leaf, CpuidRegisters& out) {
#if defined(__GNUC__) && OB_BENCH_HAS_TSC
  return __get_cpuid(leaf, &out.eax, &out.ebx, &out.ecx, &out.edx) != 0;
#else
  static_cast<void>(leaf);
  static_cast<void>(out);
  return false;
#endif
}

[[nodiscard]] unsigned cpuid_max_extended() {
#if defined(__GNUC__) && OB_BENCH_HAS_TSC
  return __get_cpuid_max(0x80000000U, nullptr);
#else
  return 0;
#endif
}

// The invariant TSC bit: CPUID leaf 0x80000007, EDX bit 8. When it is clear, the
// counter's rate follows the core's frequency, so a tick count is not a duration
// and every number derived from it is wrong in a way that varies with load.
[[nodiscard]] bool tsc_is_invariant() {
  if (cpuid_max_extended() < 0x80000007U) {
    return false;
  }
  CpuidRegisters registers;
  if (!cpuid(0x80000007U, registers)) {
    return false;
  }
  return (registers.edx & (1U << 8U)) != 0U;
}

[[nodiscard]] std::string cpu_brand_string() {
  if (cpuid_max_extended() < 0x80000004U) {
    return "unknown";
  }

  std::string brand;
  brand.reserve(48);

  for (unsigned leaf = 0x80000002U; leaf <= 0x80000004U; ++leaf) {
    CpuidRegisters registers;
    if (!cpuid(leaf, registers)) {
      return "unknown";
    }
    for (const unsigned value : {registers.eax, registers.ebx, registers.ecx, registers.edx}) {
      for (unsigned shift = 0; shift < 32U; shift += 8U) {
        const auto character = static_cast<char>((value >> shift) & 0xFFU);
        if (character != '\0') {
          brand.push_back(character);
        }
      }
    }
  }

  // The brand string is space padded on many parts.
  const std::size_t first = brand.find_first_not_of(' ');
  const std::size_t last = brand.find_last_not_of(' ');
  if (first == std::string::npos) {
    return "unknown";
  }
  return brand.substr(first, last - first + 1U);
}

// ---------------------------------------------------------------------------
// Calibration.
//
// The TSC counts at a fixed rate when it is invariant, but nothing reports what
// that rate is portably, so it is measured against steady_clock. Several short
// trials rather than one long one, with the median taken, so that a single
// scheduling interruption during calibration cannot skew every later number.
// ---------------------------------------------------------------------------

[[nodiscard]] double calibrate_ticks_per_ns() {
#if OB_BENCH_HAS_TSC
  constexpr int TRIALS = 7;
  constexpr auto INTERVAL = std::chrono::milliseconds(20);

  std::vector<double> rates;
  rates.reserve(TRIALS);

  for (int trial = 0; trial < TRIALS; ++trial) {
    unsigned aux = 0;
    const auto wall_start = std::chrono::steady_clock::now();
    const std::uint64_t tsc_start = __rdtscp(&aux);

    // Busy wait rather than sleep. A sleep hands the core away and can return
    // late by an unbounded amount, which would understate the tick rate.
    while (std::chrono::steady_clock::now() - wall_start < INTERVAL) {
    }

    const std::uint64_t tsc_end = __rdtscp(&aux);
    const auto wall_end = std::chrono::steady_clock::now();

    const auto elapsed_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());
    if (elapsed_ns <= 0.0) {
      continue;
    }
    rates.push_back(static_cast<double>(tsc_end - tsc_start) / elapsed_ns);
  }

  if (rates.empty()) {
    return 0.0;
  }

  std::ranges::sort(rates);
  return rates[rates.size() / 2U];
#else
  return 0.0;
#endif
}

[[nodiscard]] TimerInfo build_timer_info() {
  TimerInfo info;

#if !OB_BENCH_HAS_TSC
  info.source = ClockSource::steady_clock;
  info.fallback_reason = "not an x86 target, no TSC available";
  return info;
#else
  info.cpuid_available = cpuid_max_extended() >= 0x80000001U;

  if (!info.cpuid_available) {
    info.source = ClockSource::steady_clock;
    info.fallback_reason = "CPUID extended leaves unavailable, cannot verify TSC invariance";
    return info;
  }

  info.tsc_invariant = tsc_is_invariant();

  if (!info.tsc_invariant) {
    info.source = ClockSource::steady_clock;
    info.fallback_reason =
        "CPUID leaf 0x80000007 bit 8 is clear, the TSC rate follows core frequency";
    return info;
  }

  info.ticks_per_ns = calibrate_ticks_per_ns();

  if (info.ticks_per_ns <= 0.0) {
    info.source = ClockSource::steady_clock;
    info.fallback_reason = "TSC calibration produced no usable rate";
    return info;
  }

  info.source = ClockSource::invariant_tsc;
  return info;
#endif
}

}  // namespace

const TimerInfo& timer_info() {
  static const TimerInfo info = build_timer_info();
  return info;
}

double ticks_to_ns(std::uint64_t ticks) {
  const TimerInfo& info = timer_info();
  if (info.source == ClockSource::invariant_tsc && info.ticks_per_ns > 0.0) {
    return static_cast<double>(ticks) / info.ticks_per_ns;
  }
  // The fallback already counts nanoseconds.
  return static_cast<double>(ticks);
}

// ---------------------------------------------------------------------------
// Pinning.
// ---------------------------------------------------------------------------

PinInfo pin_to_cpu(int cpu) {
  PinInfo info;
  info.cpu = cpu;

  if (cpu < 0) {
    info.status = PinStatus::not_requested;
    info.detail = "no core requested, thread may migrate";
    return info;
  }

#ifdef _WIN32
  if (cpu >= 64) {
    info.status = PinStatus::failed;
    info.detail = "SetThreadAffinityMask takes a 64 bit mask, core index out of range";
    return info;
  }
  const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << static_cast<unsigned>(cpu);
  if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
    info.status = PinStatus::failed;
    info.detail = "SetThreadAffinityMask failed";
    return info;
  }
  info.status = PinStatus::pinned;
  info.detail =
      "pinned through SetThreadAffinityMask. Windows has no isolcpus equivalent, so this "
      "stops migration but does not stop other work sharing the core";
  return info;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    info.status = PinStatus::failed;
    info.detail = "sched_setaffinity failed";
    return info;
  }
  info.status = PinStatus::pinned;
  info.detail =
      "pinned through sched_setaffinity. For a genuinely quiet core, boot with "
      "isolcpus and nohz_full covering this index";
  return info;
#endif
}

// ---------------------------------------------------------------------------
// Turbo.
// ---------------------------------------------------------------------------

TurboInfo turbo_state() {
  TurboInfo info;

#ifdef _WIN32
  info.status = TurboStatus::unknown;
  info.detail = "not readable on Windows, assume frequency scaling is active";
  return info;
#else
  {
    std::ifstream no_turbo("/sys/devices/system/cpu/intel_pstate/no_turbo");
    int value = 0;
    if (no_turbo >> value) {
      info.status = value == 1 ? TurboStatus::disabled : TurboStatus::active;
      info.detail = "from /sys/devices/system/cpu/intel_pstate/no_turbo";
      return info;
    }
  }
  {
    std::ifstream boost("/sys/devices/system/cpu/cpufreq/boost");
    int value = 0;
    if (boost >> value) {
      info.status = value == 1 ? TurboStatus::active : TurboStatus::disabled;
      info.detail = "from /sys/devices/system/cpu/cpufreq/boost";
      return info;
    }
  }

  info.status = TurboStatus::unknown;
  info.detail =
      "no cpufreq or intel_pstate interface exposed, which is normal under a "
      "hypervisor such as WSL2";
  return info;
#endif
}

// ---------------------------------------------------------------------------
// Environment.
// ---------------------------------------------------------------------------

Environment capture_environment() {
  Environment environment;

  environment.cpu_brand = cpu_brand_string();
  environment.hardware_threads = std::thread::hardware_concurrency();

#ifdef _WIN32
  environment.os = "Windows";
#elif defined(__linux__)
  environment.os = "Linux";
#else
  environment.os = "POSIX";
#endif

#ifdef __clang__
  environment.compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
  environment.compiler = "GCC " __VERSION__;
#else
  environment.compiler = "unknown";
#endif

#ifdef OB_BUILD_TYPE
  environment.build_type = OB_BUILD_TYPE;
#else
  environment.build_type = "unknown";
#endif

#ifdef OB_BUILD_FLAGS
  environment.build_flags = OB_BUILD_FLAGS;
#else
  environment.build_flags = "unknown";
#endif

  return environment;
}

int print_environment(std::ostream& out,
                      const Environment& environment,
                      const TimerInfo& timer,
                      const PinInfo& pin,
                      const TurboInfo& turbo) {
  int warnings = 0;

  out << "Environment\n";
  out << "  CPU               " << environment.cpu_brand << "\n";
  out << "  Hardware threads  " << environment.hardware_threads << "\n";
  out << "  OS                " << environment.os << "\n";
  out << "  Compiler          " << environment.compiler << "\n";
  out << "  Build             " << environment.build_type << "\n";
  out << "  Flags             " << environment.build_flags << "\n";

  out << "  Clock             ";
  if (timer.source == ClockSource::invariant_tsc) {
    out << "invariant TSC, " << timer.ticks_per_ns << " ticks/ns, calibrated against "
        << "steady_clock\n";
    out << "                    CPUID leaf 0x80000007 bit 8 verified set\n";
  } else {
    out << "steady_clock fallback\n";
    out << "                    reason: " << timer.fallback_reason << "\n";
    ++warnings;
  }

  out << "  Core pinning      ";
  switch (pin.status) {
    case PinStatus::pinned:
      out << "cpu " << pin.cpu << ", " << pin.detail << "\n";
      break;
    case PinStatus::not_requested:
      out << "none. " << pin.detail << "\n";
      ++warnings;
      break;
    case PinStatus::unsupported:
      out << "unsupported on this platform. " << pin.detail << "\n";
      ++warnings;
      break;
    case PinStatus::failed:
      out << "FAILED. " << pin.detail << "\n";
      ++warnings;
      break;
  }

  out << "  Frequency scaling ";
  switch (turbo.status) {
    case TurboStatus::disabled:
      out << "turbo disabled, " << turbo.detail << "\n";
      break;
    case TurboStatus::active:
      out << "turbo ACTIVE, " << turbo.detail << "\n";
      ++warnings;
      break;
    case TurboStatus::unknown:
      out << "unknown, " << turbo.detail << "\n";
      ++warnings;
      break;
  }

  if (warnings > 0) {
    out << "\n";
    out << "  " << warnings << " condition(s) above degrade these numbers. They are printed\n";
    out << "  rather than suppressed, and no figure from this run should be quoted as a\n";
    out << "  headline without repeating them.\n";
  }

  return warnings;
}

}  // namespace ob::bench
