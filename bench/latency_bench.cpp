#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hdr/hdr_histogram.h>

#include "itch/mapped_file.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "itch/replay.hpp"
#include "json/provenance.hpp"
#include "json/writer.hpp"
#include "ob/book.hpp"

#include "harness.hpp"

// End to end per message replay latency, recorded into an HdrHistogram.
//
// Why this exists in the shape it does:
//
// **Averages are not an acceptable output.** A mean hides the tail, and in this
// domain the tail is the number that matters. Everything below reports p50, p90,
// p99, p99.9, p99.99 and max, and the mean only alongside them.
//
// **The histogram is high dynamic range on purpose.** Per message costs here span
// from a few nanoseconds for a message that is parsed and discarded to microseconds
// for one that triggers a band rebase. A fixed bucket histogram either loses the
// resolution that makes p99.9 meaningful or spends absurd memory keeping it.
// HdrHistogram holds constant relative precision across the whole range, which is
// the shape of this problem.
//
// **The breakdown by message type is the point, not decoration.** An aggregate
// p99.9 over a real feed is dominated by whichever message type is most common,
// which on NASDAQ is adds and deletes. Reporting per type is what makes it
// possible to see that, for example, a replace costs more than an add, rather than
// inferring it from a moved aggregate.
//
// **The timer's own cost is measured and reported.** rdtscp plus a fence is not
// free relative to an operation that costs tens of nanoseconds, so the harness
// measures back to back timestamp reads and prints that figure next to the
// results. It is deliberately not subtracted: subtracting a noisy estimate from
// every sample would corrupt the tail, which is the part that matters most.

namespace {

using ob::itch::MappedFile;
using ob::itch::MessageType;
using ob::itch::MessageView;

constexpr std::size_t BAND_LEVELS = ob::DEFAULT_BAND_LEVELS;

using BenchBook = ob::Book<BAND_LEVELS>;
using BenchDriver = ob::itch::ReplayDriver<BAND_LEVELS>;

struct Options {
  std::string path;
  std::string symbol = "QQQ";
  int cpu = -1;
  int warmup_runs = 1;
  int measured_runs = 3;

  // 2^17 slots, about 5 MiB of arena. Deliberately not the 2^20 that a first
  // version of this program used: BENCHMARKS.md records that a 2^20 arena puts
  // 40 MiB against an 8 MiB L3, so every figure taken that way is a DRAM latency
  // wearing an operation's name. It is an option rather than a constant because
  // the right value depends on the symbol's depth, and because the sensitivity
  // should be measurable rather than assumed.
  std::uint32_t arena_capacity = 1U << 17;

  bool help = false;
  std::string json_path;
  std::string sha256;
};

void print_usage() {
  std::cout << "ob_latency_bench, per message replay latency\n\n"
            << "  --file <path>     ITCH 5.0 capture to replay, required\n"
            << "  --symbol <sym>    symbol to reconstruct, default QQQ\n"
            << "  --cpu <n>         pin to this core, default none, which is a warning\n"
            << "  --warmup <n>      unrecorded runs before measuring, default 1\n"
            << "  --runs <n>        measured runs, default 3, more than one so that\n"
            << "                    run to run variance can be reported\n"
            << "  --arena <n>       order arena slots, default 131072. Larger is not\n"
            << "                    safer: an oversized arena measures DRAM, not the book\n"
            << "  --json <path>     also write the results to <path> as JSON\n"
            << "  --sha256 <hex>    record this hash of the input in the provenance block\n"
            << "  --help\n\n"
            << "The JSON carries the environment block as data, not as prose. A latency\n"
            << "figure without its machine is not a result, and a consumer that reads only\n"
            << "the percentiles would be publishing one.\n";
}

// strtol rather than atoi, so that a typo in a command line argument is rejected
// rather than silently becoming zero. A benchmark that quietly runs with zero
// warmup runs because someone wrote "--warmup two" is exactly the sort of silent
// degradation the rest of this harness exists to prevent.
[[nodiscard]] bool parse_int(const std::string& text, long& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];

    const auto next_value = [&](std::string& out) {
      if (index + 1 >= argc) {
        return false;
      }
      out = argv[++index];
      return true;
    };

    if (argument == "--help" || argument == "-h") {
      options.help = true;
      return true;
    }
    if (argument == "--file") {
      if (!next_value(options.path)) {
        return false;
      }
      continue;
    }
    if (argument == "--symbol") {
      if (!next_value(options.symbol)) {
        return false;
      }
      continue;
    }
    if (argument == "--json") {
      if (!next_value(options.json_path)) {
        return false;
      }
      continue;
    }
    if (argument == "--sha256") {
      if (!next_value(options.sha256)) {
        return false;
      }
      continue;
    }
    if (argument == "--cpu") {
      std::string value;
      long parsed = 0;
      if (!next_value(value) || !parse_int(value, parsed)) {
        return false;
      }
      options.cpu = static_cast<int>(parsed);
      continue;
    }
    if (argument == "--warmup") {
      std::string value;
      long parsed = 0;
      if (!next_value(value) || !parse_int(value, parsed) || parsed < 0) {
        return false;
      }
      options.warmup_runs = static_cast<int>(parsed);
      continue;
    }
    if (argument == "--runs") {
      std::string value;
      long parsed = 0;
      if (!next_value(value) || !parse_int(value, parsed) || parsed < 1) {
        return false;
      }
      options.measured_runs = static_cast<int>(parsed);
      continue;
    }
    if (argument == "--arena") {
      std::string value;
      long parsed = 0;
      if (!next_value(value) || !parse_int(value, parsed) || parsed < 1) {
        return false;
      }
      options.arena_capacity = static_cast<std::uint32_t>(parsed);
      continue;
    }

    std::cerr << "unknown argument: " << argument << "\n";
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Message type grouping.
//
// Every ITCH type the replay driver can act on gets its own histogram. Everything
// else lands in a single "parsed and discarded" bucket, which is not padding: on a
// real multi symbol capture that is the majority of messages, and its cost is what
// a filtered replay actually pays most of the time.
// ---------------------------------------------------------------------------

enum class Bucket : std::uint8_t {
  add,
  executed,
  cancel,
  del,
  replace,
  trade,
  system,
  other,
  count,
};

constexpr std::size_t BUCKET_COUNT = static_cast<std::size_t>(Bucket::count);

[[nodiscard]] const char* bucket_name(Bucket bucket) {
  switch (bucket) {
    case Bucket::add:
      return "add order (A/F)";
    case Bucket::executed:
      return "executed (E/C)";
    case Bucket::cancel:
      return "cancel (X)";
    case Bucket::del:
      return "delete (D)";
    case Bucket::replace:
      return "replace (U)";
    case Bucket::trade:
      return "trade (P/Q/B)";
    case Bucket::system:
      return "system/directory";
    case Bucket::other:
      return "parsed, discarded";
    case Bucket::count:
      break;
  }
  return "unknown";
}

[[nodiscard]] Bucket bucket_for(MessageType type) {
  switch (type) {
    case MessageType::add_order:
    case MessageType::add_order_with_mpid:
      return Bucket::add;
    case MessageType::order_executed:
    case MessageType::order_executed_with_price:
      return Bucket::executed;
    case MessageType::order_cancel:
      return Bucket::cancel;
    case MessageType::order_delete:
      return Bucket::del;
    case MessageType::order_replace:
      return Bucket::replace;
    case MessageType::trade:
    case MessageType::cross_trade:
    case MessageType::broken_trade:
      return Bucket::trade;
    case MessageType::system_event:
    case MessageType::stock_directory:
      return Bucket::system;
    default:
      return Bucket::other;
  }
}

// ---------------------------------------------------------------------------
// Histograms.
// ---------------------------------------------------------------------------

// 1 ns to 1 s, three significant figures. Three is what keeps p99.99 meaningful
// without the structure becoming large.
constexpr std::int64_t HISTOGRAM_LOWEST_NS = 1;
constexpr std::int64_t HISTOGRAM_HIGHEST_NS = 1000LL * 1000LL * 1000LL;
constexpr int HISTOGRAM_SIGNIFICANT_FIGURES = 3;

class Histograms {
 public:
  Histograms() {
    for (std::size_t index = 0; index < BUCKET_COUNT; ++index) {
      if (hdr_init(HISTOGRAM_LOWEST_NS,
                   HISTOGRAM_HIGHEST_NS,
                   HISTOGRAM_SIGNIFICANT_FIGURES,
                   &buckets_[index]) != 0) {
        buckets_[index] = nullptr;
      }
    }
    if (hdr_init(
            HISTOGRAM_LOWEST_NS, HISTOGRAM_HIGHEST_NS, HISTOGRAM_SIGNIFICANT_FIGURES, &total_) !=
        0) {
      total_ = nullptr;
    }
  }

  Histograms(const Histograms&) = delete;
  Histograms& operator=(const Histograms&) = delete;
  Histograms(Histograms&&) = delete;
  Histograms& operator=(Histograms&&) = delete;

  ~Histograms() {
    for (hdr_histogram* histogram : buckets_) {
      if (histogram != nullptr) {
        hdr_close(histogram);
      }
    }
    if (total_ != nullptr) {
      hdr_close(total_);
    }
  }

  [[nodiscard]] bool valid() const {
    if (total_ == nullptr) {
      return false;
    }
    return std::ranges::all_of(buckets_,
                               [](const hdr_histogram* histogram) { return histogram != nullptr; });
  }

  void record(Bucket bucket, std::int64_t nanoseconds) {
    // HdrHistogram's lowest trackable value is 1. A sub-nanosecond measurement is
    // real on a fast path, and clamping is the honest handling: dropping it would
    // silently remove the fastest messages from every percentile.
    const std::int64_t clamped = std::max<std::int64_t>(nanoseconds, HISTOGRAM_LOWEST_NS);
    hdr_record_value(buckets_[static_cast<std::size_t>(bucket)], clamped);
    hdr_record_value(total_, clamped);
  }

  [[nodiscard]] const hdr_histogram* bucket(Bucket value) const {
    return buckets_[static_cast<std::size_t>(value)];
  }

  [[nodiscard]] const hdr_histogram* total() const { return total_; }

 private:
  std::array<hdr_histogram*, BUCKET_COUNT> buckets_{};
  hdr_histogram* total_ = nullptr;
};

// One column per statement rather than one long chained expression. The chained
// form is formatted differently by clang-format 18 and clang-format 22, and CI
// runs 18 while this host has 22, so the concise version fails a check that
// cannot be reproduced locally. Short statements format identically under both.
void print_percentile_header() {
  std::cout << "\n";
  std::cout << std::left << std::setw(20) << "message type" << std::right;
  std::cout << std::setw(12) << "count";
  std::cout << std::setw(9) << "p50";
  std::cout << std::setw(9) << "p90";
  std::cout << std::setw(9) << "p99";
  std::cout << std::setw(10) << "p99.9";
  std::cout << std::setw(11) << "p99.99";
  std::cout << std::setw(11) << "max";
  std::cout << std::setw(9) << "mean";
  std::cout << "\n";
  std::cout << std::string(100, '-') << "\n";
}

void print_percentile_row(const char* label, const hdr_histogram* histogram) {
  const std::int64_t count = histogram->total_count;
  if (count == 0) {
    return;
  }

  std::cout << std::left << std::setw(20) << label << std::right << std::setw(12) << count
            << std::setw(9) << hdr_value_at_percentile(histogram, 50.0) << std::setw(9)
            << hdr_value_at_percentile(histogram, 90.0) << std::setw(9)
            << hdr_value_at_percentile(histogram, 99.0) << std::setw(10)
            << hdr_value_at_percentile(histogram, 99.9) << std::setw(11)
            << hdr_value_at_percentile(histogram, 99.99) << std::setw(11) << hdr_max(histogram)
            << std::setw(9) << std::fixed << std::setprecision(1) << hdr_mean(histogram)
            << std::defaultfloat << "\n";
}

// ---------------------------------------------------------------------------
// The timer's own cost, measured rather than assumed.
// ---------------------------------------------------------------------------

[[nodiscard]] double measure_timer_overhead_ns() {
  constexpr int SAMPLES = 4096;
  std::vector<double> costs;
  costs.reserve(SAMPLES);

  for (int sample = 0; sample < SAMPLES; ++sample) {
    const std::uint64_t start = ob::bench::tick_start();
    const std::uint64_t end = ob::bench::tick_end();
    costs.push_back(ob::bench::ticks_to_ns(end - start));
  }

  std::ranges::sort(costs);
  return costs[costs.size() / 2U];
}

struct RunResult {
  double elapsed_seconds = 0.0;
  std::uint64_t messages = 0;
  std::int64_t p99 = 0;
  double throughput = 0.0;
};

// One replay. The histograms accumulate across every instrumented run.
//
// Instrument is a template parameter rather than a flag because it has to leave no
// trace in the uninstrumented build of the loop. That matters more than it looks:
// **per message latency and true throughput cannot be measured in the same pass.**
// Two timestamp reads and two fences per message is a large fraction of the cost
// of a message, so a throughput figure taken from the instrumented loop is a
// figure for an instrumented engine, which is not the thing anyone wants to know.
// The latency runs and the throughput runs are therefore separate passes over the
// same data, and only the uninstrumented ones produce the throughput number.
template <bool Instrument>
RunResult replay_once(std::span<const std::byte> data,
                      const std::string& symbol,
                      std::uint32_t arena_capacity,
                      Histograms& histograms) {
  BenchBook::Config config;
  config.price = ob::PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 0};
  config.arena_capacity = arena_capacity;
  config.initial_center = ob::Ticks{0};

  BenchBook book(config);
  BenchDriver driver(book, symbol);

  std::uint64_t messages = 0;

  const auto wall_start = std::chrono::steady_clock::now();

  const ob::itch::ParseResult result =
      ob::itch::for_each_message(data, [&](const MessageView& message) {
        if constexpr (Instrument) {
          const Bucket bucket = bucket_for(message.type());

          const std::uint64_t start = ob::bench::tick_start();
          driver.apply(message);
          const std::uint64_t end = ob::bench::tick_end();

          histograms.record(
              bucket, static_cast<std::int64_t>(std::llround(ob::bench::ticks_to_ns(end - start))));
        } else {
          driver.apply(message);
        }
        ++messages;
      });

  const auto wall_end = std::chrono::steady_clock::now();
  static_cast<void>(result);

  RunResult run;
  run.elapsed_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(wall_end - wall_start).count();
  run.messages = messages;
  run.throughput =
      run.elapsed_seconds > 0.0 ? static_cast<double>(messages) / run.elapsed_seconds : 0.0;
  return run;
}

[[nodiscard]] double coefficient_of_variation(const std::vector<double>& values) {
  if (values.size() < 2U) {
    return 0.0;
  }
  double sum = 0.0;
  for (const double value : values) {
    sum += value;
  }
  const double mean = sum / static_cast<double>(values.size());
  if (mean == 0.0) {
    return 0.0;
  }

  double variance = 0.0;
  for (const double value : values) {
    variance += (value - mean) * (value - mean);
  }
  variance /= static_cast<double>(values.size() - 1U);

  return (std::sqrt(variance) / mean) * 100.0;
}

// ---------------------------------------------------------------------------
// JSON, for the results site.
//
// The environment block is written as data rather than as the prose
// print_environment produces. A consumer that read the percentiles without the
// clock source, the pinning state, and the frequency scaling state would be
// republishing a figure with its conditions stripped off, which is the exact
// failure this harness was built to prevent.
// ---------------------------------------------------------------------------

[[nodiscard]] const char* clock_source_name(ob::bench::ClockSource source) {
  switch (source) {
    case ob::bench::ClockSource::invariant_tsc:
      return "invariant_tsc";
    case ob::bench::ClockSource::steady_clock:
      return "steady_clock";
  }
  return "unknown";
}

[[nodiscard]] const char* pin_status_name(ob::bench::PinStatus status) {
  switch (status) {
    case ob::bench::PinStatus::pinned:
      return "pinned";
    case ob::bench::PinStatus::not_requested:
      return "not_requested";
    case ob::bench::PinStatus::unsupported:
      return "unsupported";
    case ob::bench::PinStatus::failed:
      return "failed";
  }
  return "unknown";
}

[[nodiscard]] const char* turbo_status_name(ob::bench::TurboStatus status) {
  switch (status) {
    case ob::bench::TurboStatus::active:
      return "active";
    case ob::bench::TurboStatus::disabled:
      return "disabled";
    case ob::bench::TurboStatus::unknown:
      return "unknown";
  }
  return "unknown";
}

void write_histogram(ob::json::Writer& writer, const char* label, const hdr_histogram* histogram) {
  writer.begin_object();
  writer.field("bucket", label);
  writer.field("count", histogram->total_count);
  writer.field("p50", hdr_value_at_percentile(histogram, 50.0));
  writer.field("p90", hdr_value_at_percentile(histogram, 90.0));
  writer.field("p99", hdr_value_at_percentile(histogram, 99.0));
  writer.field("p99_9", hdr_value_at_percentile(histogram, 99.9));
  writer.field("p99_99", hdr_value_at_percentile(histogram, 99.99));
  writer.field("max", hdr_max(histogram));
  writer.field("mean", hdr_mean(histogram), ob::json::RATIO_DECIMALS);
  writer.end_object();
}

struct JsonInputs {
  const ob::bench::Environment* environment = nullptr;
  const ob::bench::TimerInfo* timer = nullptr;
  const ob::bench::PinInfo* pin = nullptr;
  const ob::bench::TurboInfo* turbo = nullptr;
  const Histograms* histograms = nullptr;
  const std::vector<double>* throughputs = nullptr;
  double timer_overhead_ns = 0.0;
  double mean_throughput = 0.0;
  std::uint64_t messages = 0;
  int warnings = 0;
};

void write_json(const Options& options,
                const ob::json::Provenance& provenance,
                const JsonInputs& inputs) {
  // Binary rather than text, so a newline stays one byte on Windows and the file
  // is identical whichever host wrote it.
  std::ofstream file(options.json_path, std::ios::trunc | std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << options.json_path << " for writing\n";
    std::exit(1);
  }

  ob::json::Writer writer(file);
  writer.begin_object();
  ob::json::write_header(writer, provenance);

  writer.key("environment");
  writer.begin_object();
  writer.field("cpu_brand", inputs.environment->cpu_brand);
  writer.field("hardware_threads", inputs.environment->hardware_threads);
  writer.field("os", inputs.environment->os);
  writer.field("compiler", inputs.environment->compiler);
  writer.field("build_type", inputs.environment->build_type);
  writer.field("build_flags", inputs.environment->build_flags);
  writer.end_object();

  writer.key("clock");
  writer.begin_object();
  writer.field("source", clock_source_name(inputs.timer->source));
  writer.field("ticks_per_ns", inputs.timer->ticks_per_ns, ob::json::RATE_DECIMALS);
  writer.field("cpuid_available", inputs.timer->cpuid_available);
  writer.field("tsc_invariant", inputs.timer->tsc_invariant);
  writer.field("fallback_reason", inputs.timer->fallback_reason);
  writer.field("overhead_ns", inputs.timer_overhead_ns, ob::json::MONEY_DECIMALS);
  writer.field("overhead_subtracted", false);
  writer.end_object();

  writer.key("pinning");
  writer.begin_object();
  writer.field("status", pin_status_name(inputs.pin->status));
  writer.field("cpu", inputs.pin->cpu);
  writer.field("detail", inputs.pin->detail);
  writer.end_object();

  writer.key("frequency_scaling");
  writer.begin_object();
  writer.field("status", turbo_status_name(inputs.turbo->status));
  writer.field("detail", inputs.turbo->detail);
  writer.end_object();

  writer.field("degraded_conditions", inputs.warnings);

  writer.key("input");
  writer.begin_object();
  writer.field("symbol", options.symbol);
  writer.field("arena_capacity", options.arena_capacity);
  writer.field("warmup_runs", options.warmup_runs);
  writer.field("measured_runs", options.measured_runs);
  writer.field("messages_per_run", inputs.messages);
  writer.end_object();

  writer.key("latency_ns");
  writer.begin_array();
  for (std::size_t index = 0; index < BUCKET_COUNT; ++index) {
    const auto bucket = static_cast<Bucket>(index);
    const hdr_histogram* histogram = inputs.histograms->bucket(bucket);
    if (histogram->total_count == 0) {
      continue;
    }
    write_histogram(writer, bucket_name(bucket), histogram);
  }
  write_histogram(writer, "ALL", inputs.histograms->total());
  writer.end_array();

  // The max column is a scheduling event rather than a property of the book on a
  // core the operating system can interrupt, and the site has no way to know that
  // unless the artifact says so.
  writer.field("max_is_a_scheduling_event", true);

  writer.key("throughput");
  writer.begin_object();
  writer.field("mean_messages_per_second", inputs.mean_throughput, ob::json::MONEY_DECIMALS);
  writer.field("run_to_run_cv_percent",
               coefficient_of_variation(*inputs.throughputs),
               ob::json::PERCENT_DECIMALS);
  writer.field("measured_uninstrumented", true);
  writer.key("runs");
  writer.begin_array();
  for (const double value : *inputs.throughputs) {
    writer.value(value, ob::json::MONEY_DECIMALS);
  }
  writer.end_array();
  writer.end_object();

  writer.end_object();
  writer.finish();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }
  if (options.path.empty()) {
    std::cerr << "--file is required\n\n";
    print_usage();
    return 2;
  }

  // Pin before calibrating, so the calibration and every later measurement happen
  // on the same core.
  const ob::bench::PinInfo pin = ob::bench::pin_to_cpu(options.cpu);
  const ob::bench::TimerInfo& timer = ob::bench::timer_info();
  const ob::bench::TurboInfo turbo = ob::bench::turbo_state();
  const ob::bench::Environment environment = ob::bench::capture_environment();

  const int warnings = ob::bench::print_environment(std::cout, environment, timer, pin, turbo);

  MappedFile file;
  if (!file.open(options.path)) {
    std::cerr << "\ncannot open " << options.path << ": " << file.error() << "\n";
    return 1;
  }

  std::cout << "\nInput\n";
  std::cout << "  File              " << options.path << "\n";
  std::cout << "  Size              " << file.size() << " bytes\n";
  std::cout << "  Symbol            " << options.symbol << "\n";
  std::cout << "  Arena             " << options.arena_capacity << " slots, "
            << (static_cast<double>(options.arena_capacity) * sizeof(ob::Order)) / (1024.0 * 1024.0)
            << " MiB\n";
  std::cout << "  Warmup runs       " << options.warmup_runs << "\n";
  std::cout << "  Measured runs     " << options.measured_runs << "\n";

  const double timer_overhead = measure_timer_overhead_ns();
  std::cout << "  Timer overhead    " << std::fixed << std::setprecision(2) << timer_overhead
            << " ns per measured region, median of 4096 back to back reads.\n"
            << "                    Not subtracted from the results: removing a noisy\n"
            << "                    estimate from every sample would corrupt the tail.\n"
            << std::defaultfloat;

  Histograms histograms;
  if (!histograms.valid()) {
    std::cerr << "\nHdrHistogram allocation failed\n";
    return 1;
  }

  // The warmup runs a complete replay and discards it. Two things that matters
  // for: the caches and branch predictors are warm when recording starts, and the
  // allocator has already faulted in pages of the size the measured runs will ask
  // for, so a measured run reuses resident pages rather than paying first touch
  // faults inside a timed region.
  for (int run = 0; run < options.warmup_runs; ++run) {
    static_cast<void>(
        replay_once<false>(file.bytes(), options.symbol, options.arena_capacity, histograms));
  }

  // Pass one: latency. Instrumented, so every message is timed individually and
  // the throughput of these runs is not reported.
  std::uint64_t messages = 0;
  for (int run = 0; run < options.measured_runs; ++run) {
    const RunResult result =
        replay_once<true>(file.bytes(), options.symbol, options.arena_capacity, histograms);
    messages = result.messages;
  }

  // Pass two: throughput. Uninstrumented, so this is the engine's rate rather
  // than the rate of the engine plus two timestamp reads per message.
  std::vector<double> throughputs;
  throughputs.reserve(static_cast<std::size_t>(std::max(options.measured_runs, 1)));

  for (int run = 0; run < options.measured_runs; ++run) {
    const RunResult result =
        replay_once<false>(file.bytes(), options.symbol, options.arena_capacity, histograms);
    throughputs.push_back(result.throughput);
  }

  print_percentile_header();
  for (std::size_t index = 0; index < BUCKET_COUNT; ++index) {
    const auto bucket = static_cast<Bucket>(index);
    print_percentile_row(bucket_name(bucket), histograms.bucket(bucket));
  }
  std::cout << std::string(100, '-') << "\n";
  print_percentile_row("ALL", histograms.total());

  std::cout << "\nAll figures are nanoseconds. Percentiles come from HdrHistogram at three\n"
            << "significant figures across a 1 ns to 1 s range.\n";
  std::cout << "\nReading these honestly:\n"
            << "  The timer overhead above is included in every figure. Subtract it to\n"
            << "  compare against a microbenchmark that measured the same operation.\n"
            << "  The max column is not a property of the book on a core the OS can\n"
            << "  interrupt. A millisecond scale max is a scheduling event, not a rebase.\n"
            << "  p99.9 and p99.99 are the tail figures worth reading here; max needs an\n"
            << "  isolated core before it means anything.\n";

  double throughput_sum = 0.0;
  for (const double value : throughputs) {
    throughput_sum += value;
  }
  const double mean_throughput =
      throughputs.empty() ? 0.0 : throughput_sum / static_cast<double>(throughputs.size());

  std::cout << "\nThroughput, measured on separate uninstrumented runs\n";
  std::cout << "  Messages per run  " << messages << "\n";
  std::cout << "  Mean              " << std::fixed << std::setprecision(2) << mean_throughput / 1e6
            << " M msg/s\n";
  std::cout << "  Run to run CV     " << coefficient_of_variation(throughputs) << " %\n"
            << std::defaultfloat;
  std::cout << "  Runs              ";
  for (const double value : throughputs) {
    std::cout << std::fixed << std::setprecision(2) << value / 1e6 << " ";
  }
  std::cout << std::defaultfloat << "\n";

  if (warnings > 0) {
    std::cout << "\nThis run had " << warnings
              << " degraded condition(s). See the environment block above.\n";
  }

  if (!options.json_path.empty()) {
    ob::json::Provenance provenance;
    provenance.tool = "ob_latency_bench";
    provenance.version = OB_VERSION_STRING;
    provenance.source_path = options.path;
    provenance.source_bytes = file.size();
    provenance.source_sha256 = options.sha256;
    provenance.command = ob::json::command_line(argc, argv);

    JsonInputs inputs;
    inputs.environment = &environment;
    inputs.timer = &timer;
    inputs.pin = &pin;
    inputs.turbo = &turbo;
    inputs.histograms = &histograms;
    inputs.throughputs = &throughputs;
    inputs.timer_overhead_ns = timer_overhead;
    inputs.mean_throughput = mean_throughput;
    inputs.messages = messages;
    inputs.warnings = warnings;

    write_json(options, provenance, inputs);
    std::cout << "\nwrote " << options.json_path << "\n";
  }

  return 0;
}
