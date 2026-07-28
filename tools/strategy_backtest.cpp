#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "itch/mapped_file.hpp"
#include "json/provenance.hpp"
#include "json/writer.hpp"
#include "strategy/backtest.hpp"

// Runs the market making strategy over a replayed ITCH capture and reports P&L
// attribution, markouts, and the crossed book statistics.
//
// Also runs the parameter sweep, which is framed as sensitivity analysis and
// nothing more. See the note printed with the sweep output and ROADMAP.md: the
// best cell of a grid searched over one day of one symbol is a property of that
// day, and presenting it as a discovered edge would be worse than omitting the
// sweep entirely.

namespace {

using ob::strategy::Backtest;
using ob::strategy::BacktestConfig;
using ob::strategy::MarkoutSet;

struct Options {
  std::string path;
  std::string symbol = "QQQ";
  std::int32_t offset_ticks = 1;
  std::uint32_t size = 100;
  std::int64_t position_limit = 1000;
  double skew_ticks = 0.0;
  std::uint64_t latency_us = 0;
  bool sweep = false;
  bool latency_sweep = false;
  bool help = false;
  std::string json_path;
  std::string sha256;
};

void print_usage() {
  std::cout << "ob_strategy_backtest, market making over a replayed ITCH capture\n\n"
            << "  --file <path>     ITCH 5.0 capture, required\n"
            << "  --symbol <sym>    symbol to trade, default QQQ\n"
            << "  --offset <ticks>  quote distance from mid, default 1\n"
            << "  --size <shares>   quote size per side, default 100\n"
            << "  --limit <shares>  position limit, default 1000\n"
            << "  --skew <ticks>    quote shift at full inventory, default 0\n"
            << "  --latency-us <n>  reaction latency in microseconds, default 0.\n"
            << "                    Zero is an idealised bound, not a realistic setting\n"
            << "  --sweep           run the parameter sensitivity grid\n"
            << "  --latency-sweep   run the latency sensitivity grid\n"
            << "  --json <path>     also write the results to <path> as JSON\n"
            << "  --sha256 <hex>    record this hash of the input in the provenance block\n"
            << "  --help\n\n"
            << "In JSON mode --sweep and --latency-sweep can be given together, and both\n"
            << "grids land in one document. The text mode runs one or the other, because a\n"
            << "reader wants one table at a time and a file wants everything at once.\n";
}

[[nodiscard]] bool parse_long(const std::string& text, long& out) {
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

[[nodiscard]] bool parse_double(const std::string& text, double& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
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
    if (argument == "--sweep") {
      options.sweep = true;
      continue;
    }
    if (argument == "--latency-sweep") {
      options.latency_sweep = true;
      continue;
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

    std::string value;
    long parsed = 0;
    if (argument == "--offset") {
      if (!next_value(value) || !parse_long(value, parsed)) {
        return false;
      }
      options.offset_ticks = static_cast<std::int32_t>(parsed);
      continue;
    }
    if (argument == "--size") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 1) {
        return false;
      }
      options.size = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (argument == "--limit") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 1) {
        return false;
      }
      options.position_limit = parsed;
      continue;
    }
    if (argument == "--latency-us") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 0) {
        return false;
      }
      options.latency_us = static_cast<std::uint64_t>(parsed);
      continue;
    }
    if (argument == "--skew") {
      double skew = 0.0;
      if (!next_value(value) || !parse_double(value, skew)) {
        return false;
      }
      options.skew_ticks = skew;
      continue;
    }

    std::cerr << "unknown argument: " << argument << "\n";
    return false;
  }
  return true;
}

BacktestConfig config_from(const Options& options) {
  BacktestConfig config;
  config.symbol = options.symbol;
  config.maker.quote_offset_ticks = options.offset_ticks;
  config.maker.quote_size = options.size;
  config.maker.position_limit = options.position_limit;
  config.maker.inventory_skew_ticks = options.skew_ticks;
  config.latency_ns = options.latency_us * 1000ULL;
  return config;
}

void print_markout(const char* label, const MarkoutSet& set) {
  std::cout << "  " << std::left << std::setw(8) << label << std::right << std::fixed
            << std::setprecision(4) << std::setw(12) << set.buy.per_share() << std::setw(12)
            << set.sell.per_share() << std::setw(12) << set.all.per_share() << std::setw(10)
            << set.all.fills << "\n"
            << std::defaultfloat;
}

void report(const Backtest<>& backtest) {
  const auto& pnl = backtest.pnl();
  const auto& crossed = backtest.crossed();

  std::cout << "\nP&L attribution, in ticks times shares\n";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Spread capture    " << std::setw(14) << pnl.spread_pnl() << "\n";
  std::cout << "  Inventory         " << std::setw(14) << pnl.inventory_pnl() << "\n";
  std::cout << "  Fees and rebates  " << std::setw(14) << pnl.fee_pnl() << "\n";
  std::cout << "  Total             " << std::setw(14) << pnl.total_pnl() << "\n";
  std::cout << std::defaultfloat;

  std::cout << "\nRisk and activity\n";
  std::cout << "  Final position    " << pnl.position() << "\n";
  std::cout << "  Max long/short    " << pnl.max_long() << " / " << pnl.max_short() << "\n";
  std::cout << "  Time holding      " << (pnl.time_holding_ns() / 1000000ULL) << " ms\n";
  std::cout << "  Max drawdown      " << std::fixed << std::setprecision(2) << pnl.max_drawdown()
            << std::defaultfloat << "\n";
  std::cout << "  Fills             " << pnl.fill_count() << " (" << pnl.filled_shares()
            << " shares)\n";
  std::cout << "  of which through  " << pnl.trade_through_fills()
            << ", inferred from the market trading past our price\n";
  std::cout << "  Quotes placed     " << backtest.quotes_placed() << ", cancelled "
            << backtest.quotes_cancelled() << ", post-only rejected " << backtest.quotes_rejected()
            << "\n";
  std::cout << "  Cancels too late  " << backtest.cancels_too_late()
            << ", the quote had already filled when the cancel landed\n";
  std::cout << "  Limit overshoot   " << backtest.limit_overshoot_shares()
            << " shares beyond the position limit, from quotes in flight when it "
            << "was reached\n";
  std::cout << "  Fill ratio        " << std::fixed << std::setprecision(4) << pnl.fill_ratio()
            << std::defaultfloat << " of quoted shares\n";
  std::cout << "  Sharpe            " << std::fixed << std::setprecision(3) << pnl.sharpe()
            << std::defaultfloat << " on " << pnl.sample_count()
            << " one second samples, not annualised\n";
  std::cout << "                    " << pnl.informative_sample_count() << " non-zero, from "
            << pnl.fill_count() << " fills\n";
  if (!pnl.sharpe_clears_minimum_events()) {
    std::cout << "                    NOT MEANINGFUL: fewer than 30 fills, so a mean and a\n"
              << "                    standard deviation do not describe anything here.\n";
  }
  std::cout << "                    Treat as descriptive regardless. The series is dominated by\n"
            << "                    mark to market on a position held for a long time, so the\n"
            << "                    samples are strongly autocorrelated, which understates the\n"
            << "                    variance and therefore overstates the ratio.\n";

  std::cout << "\nMarkouts, signed P&L per share against mid at each horizon\n";
  std::cout << "  " << std::left << std::setw(8) << "horizon" << std::right << std::setw(12)
            << "buy" << std::setw(12) << "sell" << std::setw(12) << "all" << std::setw(10)
            << "fills" << "\n";
  for (std::size_t index = 0; index < ob::strategy::MARKOUT_COUNT; ++index) {
    print_markout(ob::strategy::MARKOUT_LABELS[index], pnl.markout(index));
  }
  std::cout << "  Unresolved at end of replay: " << pnl.unresolved_markouts()
            << ". A horizon whose fills are mostly unresolved has not been measured.\n";
  std::cout << "  Persistently negative markouts mean the strategy is being adversely\n";
  std::cout << "  selected: the market moves against every fill shortly after it happens.\n";
  std::cout << "\n  Read these against the holding time above, not on their own. The longest\n";
  std::cout << "  horizon here is 300 s. If the strategy held inventory for hours, healthy\n";
  std::cout << "  markouts say each fill was individually fine and say nothing about the\n";
  std::cout << "  position built out of them, which is where the money actually went.\n";

  std::cout << "\nCrossed book, the honesty check\n";
  std::cout << "  Crossed intervals " << crossed.intervals << "\n";
  std::cout << "  Locked, not crossed " << crossed.locked_observations
            << " observations with bid equal to ask, counted separately\n";
  std::cout << "  Crossed time      " << (crossed.total_ns / 1000000ULL) << " ms\n";
  std::cout << "  Observations      " << std::fixed << std::setprecision(4)
            << (crossed.crossed_fraction() * 100.0) << " % of book updates" << std::defaultfloat
            << "\n";
  std::cout << "  Worst depth       " << crossed.worst_depth_ticks << " ticks\n";
  std::cout << "  A crossed interval is time the quote sat inside the real spread with\n";
  std::cout << "  nobody trading against it. See DESIGN.md for why those are not fills.\n";
}

// How much the zero latency assumption was worth, measured rather than argued.
//
// Everything else in this file varies a strategy parameter. This varies an
// assumption about the world, which is the more important axis: a parameter the
// strategy chooses can be tuned, while latency is imposed on it. The values span
// a co-located participant at a few microseconds through to something well off
// the critical path at a millisecond.
// One row per cell of a sweep, computed once and rendered twice. Before this
// existed the CSV was printed from inside the loop, which meant a JSON mode would
// have had to rerun the whole grid to say the same thing in a different shape.
struct LatencyRow {
  std::uint64_t latency_us = 0;
  double total_pnl = 0.0;
  double spread_pnl = 0.0;
  double inventory_pnl = 0.0;
  std::uint64_t fills = 0;
  std::uint64_t filled_shares = 0;
  double fill_ratio = 0.0;
  std::uint64_t quotes_placed = 0;
  double markout_1s = 0.0;
  double crossed_percent = 0.0;
};

struct SweepRow {
  std::int32_t offset_ticks = 0;
  double skew_ticks = 0.0;
  std::uint32_t size = 0;
  double total_pnl = 0.0;
  double spread_pnl = 0.0;
  double inventory_pnl = 0.0;
  double fee_pnl = 0.0;
  std::uint64_t fills = 0;
  double fill_ratio = 0.0;
  double max_drawdown = 0.0;
  double markout_1s = 0.0;
  double crossed_percent = 0.0;
};

[[nodiscard]] std::vector<LatencyRow> latency_sweep_rows(std::span<const std::byte> data,
                                                         const Options& options) {
  const std::vector<std::uint64_t> latencies_us{0, 1, 5, 25, 100, 500, 1000};

  std::vector<LatencyRow> rows;
  rows.reserve(latencies_us.size());

  for (const std::uint64_t latency : latencies_us) {
    Options local = options;
    local.latency_us = latency;

    Backtest<> backtest(config_from(local));
    backtest.run(data);

    const auto& pnl = backtest.pnl();
    rows.push_back(LatencyRow{.latency_us = latency,
                              .total_pnl = pnl.total_pnl(),
                              .spread_pnl = pnl.spread_pnl(),
                              .inventory_pnl = pnl.inventory_pnl(),
                              .fills = pnl.fill_count(),
                              .filled_shares = pnl.filled_shares(),
                              .fill_ratio = pnl.fill_ratio(),
                              .quotes_placed = backtest.quotes_placed(),
                              .markout_1s = pnl.markout_1s().all.per_share(),
                              .crossed_percent = backtest.crossed().crossed_fraction() * 100.0});
  }

  return rows;
}

[[nodiscard]] std::vector<SweepRow> sweep_rows(std::span<const std::byte> data,
                                               const Options& options) {
  const std::vector<std::int32_t> offsets{1, 2, 4};
  const std::vector<double> skews{0.0, 1.0, 2.0};
  const std::vector<std::uint32_t> sizes{100, 300};

  std::vector<SweepRow> rows;
  rows.reserve(offsets.size() * skews.size() * sizes.size());

  for (const std::int32_t offset : offsets) {
    for (const double skew : skews) {
      for (const std::uint32_t size : sizes) {
        Options local = options;
        local.offset_ticks = offset;
        local.skew_ticks = skew;
        local.size = size;

        Backtest<> backtest(config_from(local));
        backtest.run(data);

        const auto& pnl = backtest.pnl();
        rows.push_back(SweepRow{.offset_ticks = offset,
                                .skew_ticks = skew,
                                .size = size,
                                .total_pnl = pnl.total_pnl(),
                                .spread_pnl = pnl.spread_pnl(),
                                .inventory_pnl = pnl.inventory_pnl(),
                                .fee_pnl = pnl.fee_pnl(),
                                .fills = pnl.fill_count(),
                                .fill_ratio = pnl.fill_ratio(),
                                .max_drawdown = pnl.max_drawdown(),
                                .markout_1s = pnl.markout_1s().all.per_share(),
                                .crossed_percent = backtest.crossed().crossed_fraction() * 100.0});
      }
    }
  }

  return rows;
}

void print_latency_sweep(const std::vector<LatencyRow>& rows) {
  std::cout << "\nlatency_us,total_pnl,spread,inventory,fills,filled_shares,fill_ratio,"
            << "quotes_placed,markout_1s_per_share,crossed_pct\n";

  for (const LatencyRow& row : rows) {
    std::cout << row.latency_us << "," << std::fixed << std::setprecision(2) << row.total_pnl << ","
              << row.spread_pnl << "," << row.inventory_pnl << "," << row.fills << ","
              << row.filled_shares << "," << std::setprecision(4) << row.fill_ratio << ","
              << row.quotes_placed << "," << row.markout_1s << "," << row.crossed_percent
              << std::defaultfloat << "\n";
  }

  std::cout << "\nZero is the idealised bound and is not a realistic setting for any real\n"
            << "participant.\n\n"
            << "Read the fill count and the markout together, because separately they say\n"
            << "opposite things. Fills rise as latency grows, which looks like latency\n"
            << "helping and is not: the cancel is delayed as well as the placement, so a\n"
            << "quote the strategy has already decided to pull stays resting and fillable\n"
            << "for the whole window. More exposure, more fills.\n\n"
            << "The 1 s markout is what those extra fills are worth, and it falls as latency\n"
            << "rises. That is adverse selection appearing exactly where theory says it\n"
            << "should: the trades you cannot pull away from are disproportionately the ones\n"
            << "you would most have wanted to.\n\n"
            << "Total P&L is not monotonic in latency and should not be read as if it were.\n"
            << "It is dominated by inventory, which is path dependent: which fills land in\n"
            << "which order decides the position carried through the session, and that\n"
            << "wobbles rather than trends as the window widens.\n";
}

void print_sweep(const std::vector<SweepRow>& rows) {
  std::cout << "\noffset_ticks,skew_ticks,size,total_pnl,spread,inventory,fees,fills,"
            << "fill_ratio,max_drawdown,markout_1s_per_share,crossed_pct\n";

  for (const SweepRow& row : rows) {
    std::cout << row.offset_ticks << "," << row.skew_ticks << "," << row.size << "," << std::fixed
              << std::setprecision(2) << row.total_pnl << "," << row.spread_pnl << ","
              << row.inventory_pnl << "," << row.fee_pnl << "," << row.fills << ","
              << std::setprecision(4) << row.fill_ratio << "," << std::setprecision(2)
              << row.max_drawdown << "," << std::setprecision(4) << row.markout_1s << ","
              << row.crossed_percent << std::defaultfloat << "\n";
  }

  std::cout << "\nThis is sensitivity analysis, not a search for an edge. The grid covers one\n"
            << "symbol on one day, so the best cell is a property of that day and nothing\n"
            << "more. What is worth reading is the shape: whether P&L degrades smoothly as\n"
            << "the quote widens, whether skew reduces drawdown, and whether the markout\n"
            << "stays negative everywhere, which would say the fills are adversely selected\n"
            << "regardless of parameters.\n";
}

// ---------------------------------------------------------------------------
// JSON, for the results site.
// ---------------------------------------------------------------------------

// Binary rather than text. On Windows a text mode stream expands every newline to
// a carriage return and a line feed, which would make the same artifact two
// different files depending on which host wrote it and fail the CI guard that
// compares bytes.
[[nodiscard]] std::ofstream open_output(const std::string& path) {
  std::ofstream file(path, std::ios::trunc | std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << " for writing\n";
    std::exit(1);
  }
  return file;
}

void write_config(ob::json::Writer& writer, const Options& options) {
  writer.key("config");
  writer.begin_object();
  writer.field("symbol", options.symbol);
  writer.field("quote_offset_ticks", options.offset_ticks);
  writer.field("quote_size", options.size);
  writer.field("position_limit", options.position_limit);
  writer.field("inventory_skew_ticks", options.skew_ticks, ob::json::RATIO_DECIMALS);
  writer.field("latency_us", options.latency_us);
  writer.end_object();
}

void write_markouts(ob::json::Writer& writer, const ob::strategy::PnlAccount& pnl) {
  writer.key("markouts");
  writer.begin_array();
  for (std::size_t index = 0; index < ob::strategy::MARKOUT_COUNT; ++index) {
    const MarkoutSet& set = pnl.markout(index);
    writer.begin_object();
    writer.field("horizon", ob::strategy::MARKOUT_LABELS[index]);
    writer.field("horizon_ns", ob::strategy::MARKOUT_HORIZONS_NS[index]);
    writer.field("buy_per_share", set.buy.per_share(), ob::json::RATE_DECIMALS);
    writer.field("sell_per_share", set.sell.per_share(), ob::json::RATE_DECIMALS);
    writer.field("all_per_share", set.all.per_share(), ob::json::RATE_DECIMALS);
    writer.field("fills", set.all.fills);
    writer.end_object();
  }
  writer.end_array();
  writer.field("unresolved_markouts", pnl.unresolved_markouts());
}

void write_baseline(ob::json::Writer& writer, const Backtest<>& backtest) {
  const auto& pnl = backtest.pnl();
  const auto& crossed = backtest.crossed();
  const auto& stats = backtest.replay_stats();

  writer.key("replay");
  writer.begin_object();
  writer.field("messages_seen", stats.messages_seen);
  writer.field("messages_applied", stats.messages_applied);
  writer.end_object();

  writer.key("pnl");
  writer.begin_object();
  writer.field("spread", pnl.spread_pnl(), ob::json::MONEY_DECIMALS);
  writer.field("inventory", pnl.inventory_pnl(), ob::json::MONEY_DECIMALS);
  writer.field("fees", pnl.fee_pnl(), ob::json::MONEY_DECIMALS);
  writer.field("total", pnl.total_pnl(), ob::json::MONEY_DECIMALS);
  writer.field("unit", "ticks times shares");
  writer.end_object();

  writer.key("risk");
  writer.begin_object();
  writer.field("final_position", pnl.position());
  writer.field("max_long", pnl.max_long());
  writer.field("max_short", pnl.max_short());
  writer.field("time_holding_ns", pnl.time_holding_ns());
  writer.field("max_drawdown", pnl.max_drawdown(), ob::json::MONEY_DECIMALS);
  writer.end_object();

  writer.key("activity");
  writer.begin_object();
  writer.field("fills", pnl.fill_count());
  writer.field("filled_shares", pnl.filled_shares());
  writer.field("quoted_shares", pnl.quoted_shares());
  writer.field("trade_through_fills", pnl.trade_through_fills());
  writer.field("quotes_placed", backtest.quotes_placed());
  writer.field("quotes_cancelled", backtest.quotes_cancelled());
  writer.field("quotes_rejected", backtest.quotes_rejected());
  writer.field("cancels_too_late", backtest.cancels_too_late());
  writer.field("limit_overshoot_shares", backtest.limit_overshoot_shares());
  writer.field("fill_ratio", pnl.fill_ratio(), ob::json::RATIO_DECIMALS);
  writer.end_object();

  // The caveats travel with the number rather than beside it. A Sharpe ratio
  // over a strongly autocorrelated series is not wrong so much as not the thing
  // it looks like, and a site that read the value without the flags would
  // present it as if it were.
  writer.key("sharpe");
  writer.begin_object();
  writer.field("value", pnl.sharpe(), ob::json::RATIO_DECIMALS);
  writer.field("samples", pnl.sample_count());
  writer.field("informative_samples", pnl.informative_sample_count());
  writer.field("clears_minimum_events", pnl.sharpe_clears_minimum_events());
  writer.field("annualised", false);
  writer.end_object();

  write_markouts(writer, pnl);

  writer.key("crossed_book");
  writer.begin_object();
  writer.field("intervals", crossed.intervals);
  writer.field("total_ns", crossed.total_ns);
  writer.field("observations", crossed.observations);
  writer.field("crossed_observations", crossed.crossed_observations);
  writer.field("locked_observations", crossed.locked_observations);
  writer.field("worst_depth_ticks", crossed.worst_depth_ticks);
  writer.field("crossed_percent", crossed.crossed_fraction() * 100.0, ob::json::PERCENT_DECIMALS);
  writer.end_object();
}

void write_sweeps(ob::json::Writer& writer,
                  const std::vector<SweepRow>& parameters,
                  const std::vector<LatencyRow>& latencies) {
  if (!parameters.empty()) {
    writer.key("parameter_sweep");
    writer.begin_array();
    for (const SweepRow& row : parameters) {
      writer.begin_object();
      writer.field("offset_ticks", row.offset_ticks);
      writer.field("skew_ticks", row.skew_ticks, ob::json::RATIO_DECIMALS);
      writer.field("size", row.size);
      writer.field("total_pnl", row.total_pnl, ob::json::MONEY_DECIMALS);
      writer.field("spread", row.spread_pnl, ob::json::MONEY_DECIMALS);
      writer.field("inventory", row.inventory_pnl, ob::json::MONEY_DECIMALS);
      writer.field("fees", row.fee_pnl, ob::json::MONEY_DECIMALS);
      writer.field("fills", row.fills);
      writer.field("fill_ratio", row.fill_ratio, ob::json::RATIO_DECIMALS);
      writer.field("max_drawdown", row.max_drawdown, ob::json::MONEY_DECIMALS);
      writer.field("markout_1s_per_share", row.markout_1s, ob::json::RATE_DECIMALS);
      writer.field("crossed_percent", row.crossed_percent, ob::json::PERCENT_DECIMALS);
      writer.end_object();
    }
    writer.end_array();
  }

  if (latencies.empty()) {
    return;
  }

  writer.key("latency_sweep");
  writer.begin_array();
  for (const LatencyRow& row : latencies) {
    writer.begin_object();
    writer.field("latency_us", row.latency_us);
    writer.field("total_pnl", row.total_pnl, ob::json::MONEY_DECIMALS);
    writer.field("spread", row.spread_pnl, ob::json::MONEY_DECIMALS);
    writer.field("inventory", row.inventory_pnl, ob::json::MONEY_DECIMALS);
    writer.field("fills", row.fills);
    writer.field("filled_shares", row.filled_shares);
    writer.field("fill_ratio", row.fill_ratio, ob::json::RATIO_DECIMALS);
    writer.field("quotes_placed", row.quotes_placed);
    writer.field("markout_1s_per_share", row.markout_1s, ob::json::RATE_DECIMALS);
    writer.field("crossed_percent", row.crossed_percent, ob::json::PERCENT_DECIMALS);
    writer.end_object();
  }
  writer.end_array();
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

  ob::itch::MappedFile file;
  if (!file.open(options.path)) {
    std::cerr << "cannot open " << options.path << ": " << file.error() << "\n";
    return 1;
  }

  ob::json::Provenance provenance;
  provenance.tool = "ob_strategy_backtest";
  provenance.version = OB_VERSION_STRING;
  provenance.source_path = options.path;
  provenance.source_bytes = file.size();
  provenance.source_sha256 = options.sha256;
  provenance.command = ob::json::command_line(argc, argv);

  std::cout << "Backtest\n";
  std::cout << "  File              " << options.path << "\n";
  std::cout << "  Symbol            " << options.symbol << "\n";

  if (options.latency_sweep || options.sweep) {
    // Both grids can be asked for at once, which the text mode never allowed.
    // A reader wants one table at a time; a file wants everything the site will
    // need, so that one invocation produces one artifact.
    const std::vector<SweepRow> parameters =
        options.sweep ? sweep_rows(file.bytes(), options) : std::vector<SweepRow>{};
    const std::vector<LatencyRow> latencies = options.latency_sweep
                                                  ? latency_sweep_rows(file.bytes(), options)
                                                  : std::vector<LatencyRow>{};

    if (options.sweep) {
      print_sweep(parameters);
    }
    if (options.latency_sweep) {
      print_latency_sweep(latencies);
    }

    if (!options.json_path.empty()) {
      std::ofstream out = open_output(options.json_path);
      ob::json::Writer writer(out);
      writer.begin_object();
      ob::json::write_header(writer, provenance);
      write_config(writer, options);
      write_sweeps(writer, parameters, latencies);
      writer.end_object();
      writer.finish();
      std::cout << "\nwrote " << options.json_path << "\n";
    }
    return 0;
  }

  std::cout << "  Quote offset      " << options.offset_ticks << " ticks\n";
  std::cout << "  Quote size        " << options.size << " shares\n";
  std::cout << "  Position limit    " << options.position_limit << " shares\n";
  std::cout << "  Inventory skew    " << options.skew_ticks << " ticks at full inventory\n";
  std::cout << "  Reaction latency  " << options.latency_us << " us";
  if (options.latency_us == 0) {
    std::cout << ", an idealised bound rather than a realistic setting";
  }
  std::cout << "\n";

  Backtest<> backtest(config_from(options));
  backtest.run(file.bytes());

  const auto& stats = backtest.replay_stats();
  std::cout << "  Messages applied  " << stats.messages_applied << " of " << stats.messages_seen
            << "\n";

  report(backtest);

  if (!options.json_path.empty()) {
    std::ofstream out = open_output(options.json_path);
    ob::json::Writer writer(out);
    writer.begin_object();
    ob::json::write_header(writer, provenance);
    write_config(writer, options);
    write_baseline(writer, backtest);
    writer.end_object();
    writer.finish();
    std::cout << "\nwrote " << options.json_path << "\n";
  }

  return 0;
}
