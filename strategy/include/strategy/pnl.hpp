#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

#include "ob/types.hpp"

#include "strategy/queue_position.hpp"

namespace ob::strategy {

// Fees in ticks per share, signed. A maker rebate is negative because it is
// received rather than paid, which keeps the sign convention of the P&L
// decomposition uniform: every component is added, none is subtracted.
struct FeeSchedule {
  double maker_rebate_per_share = -0.002;
  double taker_fee_per_share = 0.003;
};

// The markout horizons. Signed P&L at each, measured from the
// fill price against the mid at that horizon.
constexpr std::uint64_t MARKOUT_100MS_NS = 100ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t MARKOUT_1S_NS = 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint64_t MARKOUT_10S_NS = 10ULL * 1000ULL * 1000ULL * 1000ULL;

struct MarkoutBucket {
  double total = 0.0;
  std::uint64_t fills = 0;
  std::uint64_t shares = 0;

  [[nodiscard]] double per_share() const {
    return shares == 0 ? 0.0 : total / static_cast<double>(shares);
  }
};

// Markouts split by the side the strategy took, because that is the split that
// tells you what is happening. A market maker that is picked off shows negative
// markouts on both sides; one with a directional bias shows it on one.
struct MarkoutSet {
  MarkoutBucket buy;
  MarkoutBucket sell;
  MarkoutBucket all;
};

// Profit and loss, decomposed.
//
// The decomposition is exact rather than approximate, and that is worth stating
// because most attributions are not. Total mark to market P&L is
// `cash + position * mid`. Two things move it and only two:
//
//   At a fill, with mid held still, the change is
//   `signed_quantity * (mid - fill_price)`. That is edge captured against the
//   mid at the moment of trading, which is spread capture.
//
//   Between fills, with position held still, the change is
//   `position * delta_mid`. That is inventory, or directional, P&L.
//
// Fees are the third term and are booked as they occur. So
// `total == spread + inventory + fees` holds to floating point exactly, and
// `StrategyPnl.AttributionIsExact` asserts it rather than trusting it.
class PnlAccount {
 public:
  explicit PnlAccount(FeeSchedule fees) : fees_(fees) {}

  // Called on every book update, before any fill for that update is booked.
  void mark(double mid, std::uint64_t timestamp_ns) {
    if (has_mark_) {
      inventory_pnl_ += static_cast<double>(position_) * (mid - mid_);
    }
    mid_ = mid;
    has_mark_ = true;
    last_timestamp_ns_ = timestamp_ns;

    resolve_markouts(timestamp_ns);

    if (position_ > max_long_) {
      max_long_ = position_;
    }
    if (position_ < max_short_) {
      max_short_ = position_;
    }

    const double total = total_pnl();
    if (total > peak_) {
      peak_ = total;
    }
    const double drawdown = peak_ - total;
    if (drawdown > max_drawdown_) {
      max_drawdown_ = drawdown;
    }

    if (position_ != 0) {
      time_holding_ns_ +=
          timestamp_ns > previous_ns_ && previous_ns_ != 0 ? timestamp_ns - previous_ns_ : 0;
    }
    previous_ns_ = timestamp_ns;
  }

  // Books a fill. `is_maker` decides which side of the fee schedule applies.
  void on_fill(const Fill& fill, bool is_maker) {
    const auto quantity = static_cast<double>(fill.quantity);
    const double signed_quantity = fill.side == Side::buy ? quantity : -quantity;
    const auto price = static_cast<double>(fill.price.raw());

    spread_pnl_ += signed_quantity * (mid_ - price);

    const double fee_rate = is_maker ? fees_.maker_rebate_per_share : fees_.taker_fee_per_share;
    fee_pnl_ -= fee_rate * quantity;

    position_ += fill.side == Side::buy ? static_cast<std::int64_t>(fill.quantity)
                                        : -static_cast<std::int64_t>(fill.quantity);

    ++fill_count_;
    filled_shares_ += fill.quantity;
    if (fill.from_trade_through) {
      ++trade_through_fills_;
    }

    pending_.push_back(PendingMarkout{.fill = fill, .signed_quantity = signed_quantity});
  }

  void on_quote_submitted(std::uint64_t shares) { quoted_shares_ += shares; }

  // Samples the P&L series at a fixed interval, so that Sharpe has a stated
  // sampling basis rather than an implied one.
  void sample_if_due(std::uint64_t timestamp_ns, std::uint64_t interval_ns) {
    if (last_sample_ns_ == 0) {
      last_sample_ns_ = timestamp_ns;
      last_sample_pnl_ = total_pnl();
      return;
    }
    while (timestamp_ns >= last_sample_ns_ + interval_ns) {
      last_sample_ns_ += interval_ns;
      const double current = total_pnl();
      samples_.push_back(current - last_sample_pnl_);
      last_sample_pnl_ = current;
    }
  }

  [[nodiscard]] double total_pnl() const { return spread_pnl_ + inventory_pnl_ + fee_pnl_; }

  [[nodiscard]] double spread_pnl() const noexcept { return spread_pnl_; }

  [[nodiscard]] double inventory_pnl() const noexcept { return inventory_pnl_; }

  [[nodiscard]] double fee_pnl() const noexcept { return fee_pnl_; }

  [[nodiscard]] std::int64_t position() const noexcept { return position_; }

  [[nodiscard]] std::int64_t max_long() const noexcept { return max_long_; }

  [[nodiscard]] std::int64_t max_short() const noexcept { return max_short_; }

  [[nodiscard]] double max_drawdown() const noexcept { return max_drawdown_; }

  [[nodiscard]] std::uint64_t fill_count() const noexcept { return fill_count_; }

  [[nodiscard]] std::uint64_t filled_shares() const noexcept { return filled_shares_; }

  [[nodiscard]] std::uint64_t quoted_shares() const noexcept { return quoted_shares_; }

  [[nodiscard]] std::uint64_t trade_through_fills() const noexcept { return trade_through_fills_; }

  [[nodiscard]] std::uint64_t time_holding_ns() const noexcept { return time_holding_ns_; }

  [[nodiscard]] double mid() const noexcept { return mid_; }

  [[nodiscard]] double fill_ratio() const {
    return quoted_shares_ == 0
               ? 0.0
               : static_cast<double>(filled_shares_) / static_cast<double>(quoted_shares_);
  }

  [[nodiscard]] const MarkoutSet& markout_100ms() const noexcept { return markout_100ms_; }

  [[nodiscard]] const MarkoutSet& markout_1s() const noexcept { return markout_1s_; }

  [[nodiscard]] const MarkoutSet& markout_10s() const noexcept { return markout_10s_; }

  // Sharpe over the sampled P&L series, not annualised. Annualising a backtest
  // that covers part of one trading day would be arithmetic dressed up as a
  // result, so the caller is told the sampling interval and left to draw its own
  // conclusion.
  [[nodiscard]] double sharpe() const {
    if (samples_.size() < 2U) {
      return 0.0;
    }
    double sum = 0.0;
    for (const double sample : samples_) {
      sum += sample;
    }
    const double mean = sum / static_cast<double>(samples_.size());

    double variance = 0.0;
    for (const double sample : samples_) {
      variance += (sample - mean) * (sample - mean);
    }
    variance /= static_cast<double>(samples_.size() - 1U);

    const double deviation = std::sqrt(variance);
    return deviation == 0.0 ? 0.0 : mean / deviation;
  }

  [[nodiscard]] std::size_t sample_count() const noexcept { return samples_.size(); }

  // Markouts still waiting for their horizon to elapse when the replay ended.
  // Reported rather than silently dropped: a run whose 10 s markout is mostly
  // unresolved has not measured a 10 s markout.
  [[nodiscard]] std::size_t unresolved_markouts() const noexcept { return pending_.size(); }

 private:
  struct PendingMarkout {
    Fill fill;
    double signed_quantity = 0.0;
    bool done_100ms = false;
    bool done_1s = false;
    bool done_10s = false;
  };

  static void record(MarkoutSet& set, const PendingMarkout& pending, double value) {
    MarkoutBucket& side_bucket = pending.fill.side == Side::buy ? set.buy : set.sell;
    side_bucket.total += value;
    ++side_bucket.fills;
    side_bucket.shares += pending.fill.quantity;
    set.all.total += value;
    ++set.all.fills;
    set.all.shares += pending.fill.quantity;
  }

  void resolve_markouts(std::uint64_t now_ns) {
    while (!pending_.empty()) {
      PendingMarkout& pending = pending_.front();
      const std::uint64_t elapsed =
          now_ns > pending.fill.timestamp_ns ? now_ns - pending.fill.timestamp_ns : 0;

      const auto fill_price = static_cast<double>(pending.fill.price.raw());
      const double value = pending.signed_quantity * (mid_ - fill_price);

      if (!pending.done_100ms && elapsed >= MARKOUT_100MS_NS) {
        record(markout_100ms_, pending, value);
        pending.done_100ms = true;
      }
      if (!pending.done_1s && elapsed >= MARKOUT_1S_NS) {
        record(markout_1s_, pending, value);
        pending.done_1s = true;
      }
      if (!pending.done_10s && elapsed >= MARKOUT_10S_NS) {
        record(markout_10s_, pending, value);
        pending.done_10s = true;
      }

      if (pending.done_10s) {
        pending_.pop_front();
        continue;
      }
      // Fills are appended in timestamp order, so once the front is not ready
      // neither is anything behind it.
      if (!pending.done_100ms) {
        return;
      }
      // The front still owes a longer horizon. Walk the rest for their shorter
      // ones rather than blocking behind it.
      resolve_tail(now_ns);
      return;
    }
  }

  void resolve_tail(std::uint64_t now_ns) {
    for (std::size_t index = 1; index < pending_.size(); ++index) {
      PendingMarkout& pending = pending_[index];
      const std::uint64_t elapsed =
          now_ns > pending.fill.timestamp_ns ? now_ns - pending.fill.timestamp_ns : 0;
      if (elapsed < MARKOUT_100MS_NS) {
        return;
      }

      const auto fill_price = static_cast<double>(pending.fill.price.raw());
      const double value = pending.signed_quantity * (mid_ - fill_price);

      if (!pending.done_100ms) {
        record(markout_100ms_, pending, value);
        pending.done_100ms = true;
      }
      if (!pending.done_1s && elapsed >= MARKOUT_1S_NS) {
        record(markout_1s_, pending, value);
        pending.done_1s = true;
      }
      if (!pending.done_10s && elapsed >= MARKOUT_10S_NS) {
        record(markout_10s_, pending, value);
        pending.done_10s = true;
      }
    }
  }

  FeeSchedule fees_;

  double spread_pnl_ = 0.0;
  double inventory_pnl_ = 0.0;
  double fee_pnl_ = 0.0;

  double mid_ = 0.0;
  bool has_mark_ = false;

  std::int64_t position_ = 0;
  std::int64_t max_long_ = 0;
  std::int64_t max_short_ = 0;

  double peak_ = 0.0;
  double max_drawdown_ = 0.0;

  std::uint64_t fill_count_ = 0;
  std::uint64_t filled_shares_ = 0;
  std::uint64_t quoted_shares_ = 0;
  std::uint64_t trade_through_fills_ = 0;

  std::uint64_t last_timestamp_ns_ = 0;
  std::uint64_t previous_ns_ = 0;
  std::uint64_t time_holding_ns_ = 0;

  std::uint64_t last_sample_ns_ = 0;
  double last_sample_pnl_ = 0.0;
  std::vector<double> samples_;

  std::deque<PendingMarkout> pending_;
  MarkoutSet markout_100ms_;
  MarkoutSet markout_1s_;
  MarkoutSet markout_10s_;
};

}  // namespace ob::strategy
