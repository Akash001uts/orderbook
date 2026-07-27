#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "ob/types.hpp"

namespace ob::strategy {

struct MarketMakerConfig {
  // Distance from mid to quote, in ticks, before any skew.
  std::int32_t quote_offset_ticks = 1;

  // Shares per side.
  std::uint32_t quote_size = 100;

  // Hard inventory bound. The strategy stops quoting the side that would push it
  // further past this.
  std::int64_t position_limit = 1000;

  // Ticks of quote shift at full inventory. A long position shifts both quotes
  // down, which makes the ask more attractive and the bid less, so inventory is
  // encouraged back toward flat. Zero disables skew entirely, which is the
  // baseline the sweep measures against.
  double inventory_skew_ticks = 0.0;

  // Requote when the mid has moved at least this far from where it was when the
  // current quotes were placed. One tick means requote on any mid change, which
  // is the most active setting and the most expensive in cancels.
  std::int32_t requote_mid_move_ticks = 1;
};

struct QuoteIntent {
  bool want_bid = false;
  Ticks bid_price{};
  bool want_ask = false;
  Ticks ask_price{};
};

// A two sided quoter with inventory skew.
//
// Deliberately simple. I wanted this phase to be simple and correct rather than a
// research platform, and the interesting work in Phase 5 is
// the queue position estimate, the fill model, and the honesty of the accounting.
// A more elaborate strategy would add parameters to fit, not evidence.
//
// Quotes are submitted post-only. Two reasons, and the second is the one that
// matters. A market maker that crosses the spread is paying the spread it exists
// to earn, so taking is a bug rather than a feature here. And post-only means the
// strategy can never itself create the crossed book described in DESIGN.md: a
// quote that would cross is rejected and simply not placed, so every crossed
// interval in a result is caused by a replayed venue add landing through a
// resting quote, which is the case that decision is about.
class MarketMaker {
 public:
  explicit MarketMaker(MarketMakerConfig config) : config_(config) {}

  [[nodiscard]] const MarketMakerConfig& config() const noexcept { return config_; }

  // Computes the quotes this strategy wants given the venue's best prices and the
  // current inventory. Venue best prices exclude the strategy's own resting
  // orders; quoting off a mid the strategy itself moved would be self
  // referential, and it is the kind of feedback loop that makes a backtest look
  // profitable for no reason.
  [[nodiscard]] QuoteIntent desired(std::optional<Ticks> venue_bid,
                                    std::optional<Ticks> venue_ask,
                                    std::int64_t position) const {
    QuoteIntent intent;
    if (!venue_bid.has_value() || !venue_ask.has_value()) {
      // No two sided market to quote around. Standing in an empty book would be
      // quoting at a price the data cannot justify.
      return intent;
    }

    const auto bid = static_cast<double>(venue_bid->raw());
    const auto ask = static_cast<double>(venue_ask->raw());
    if (bid >= ask) {
      // The venue book itself is crossed or locked, which on a reconstructed
      // book means something upstream is wrong rather than that there is an
      // opportunity. Do not quote into it.
      return intent;
    }

    const double mid = (bid + ask) / 2.0;
    const double skew = skew_ticks(position);
    const auto offset = static_cast<double>(config_.quote_offset_ticks);

    const auto bid_target = static_cast<std::int32_t>(std::floor(mid - offset + skew));
    const auto ask_target = static_cast<std::int32_t>(std::ceil(mid + offset + skew));

    if (bid_target >= ask_target) {
      return intent;
    }

    // The limit is tested against the position the quote would produce if it
    // filled completely, not against the current one. Testing the current
    // position lets a quote placed at 999 of a 1000 limit fill for its whole size
    // and settle at 1099, which is a risk control that does not control the risk.
    const auto size = static_cast<std::int64_t>(config_.quote_size);

    if (position + size <= config_.position_limit) {
      intent.want_bid = true;
      intent.bid_price = Ticks{bid_target};
    }
    if (position - size >= -config_.position_limit) {
      intent.want_ask = true;
      intent.ask_price = Ticks{ask_target};
    }

    return intent;
  }

  // The skew is negative when long, which shifts both quotes down.
  [[nodiscard]] double skew_ticks(std::int64_t position) const {
    if (config_.position_limit <= 0 || config_.inventory_skew_ticks == 0.0) {
      return 0.0;
    }
    double ratio = static_cast<double>(position) / static_cast<double>(config_.position_limit);
    ratio = ratio > 1.0 ? 1.0 : ratio;
    ratio = ratio < -1.0 ? -1.0 : ratio;
    return -config_.inventory_skew_ticks * ratio;
  }

 private:
  MarketMakerConfig config_;
};

}  // namespace ob::strategy
