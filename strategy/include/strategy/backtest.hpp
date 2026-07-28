#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "itch/replay.hpp"
#include "ob/engine.hpp"
#include "ob/events.hpp"

#include "strategy/market_maker.hpp"
#include "strategy/pnl.hpp"
#include "strategy/queue_position.hpp"

namespace ob::strategy {

// How long the combined book spent crossed, and how badly.
//
// This is a headline output rather than a diagnostic. See DESIGN.md, "The crossed
// book, and why the strategy tolerates one": a crossed interval is a period where
// the strategy's quote sat inside the real spread and nobody traded with it, which
// is exactly where an optimistic backtest would hide. Reporting it turns an
// unfalsifiable assumption into a number a reader can check.
struct CrossedBookStats {
  std::uint64_t intervals = 0;
  std::uint64_t total_ns = 0;
  std::int32_t worst_depth_ticks = 0;
  std::uint64_t observations = 0;
  std::uint64_t crossed_observations = 0;

  // Bid equal to ask. A different state from crossed, legal at several venues,
  // and counted separately so it cannot inflate the crossed figure.
  std::uint64_t locked_observations = 0;

  [[nodiscard]] double crossed_fraction() const {
    return observations == 0
               ? 0.0
               : static_cast<double>(crossed_observations) / static_cast<double>(observations);
  }
};

struct BacktestConfig {
  std::string symbol = "QQQ";
  MarketMakerConfig maker;
  FeeSchedule fees;
  // Deliberately larger than the library default of 16 384.
  //
  // The backtest is the one consumer that cannot use the derived default safely.
  // That default is sized to one liquid ETF's measured peak, and this tool is
  // routinely pointed at arbitrary symbols on arbitrary days through `--symbol`,
  // where the peak is unknown before the run. Exhausting the arena mid-replay
  // would silently truncate the venue book and quietly corrupt every P&L number
  // downstream, which is far worse here than the cache cost of over-provisioning.
  //
  // The strategy itself adds at most two orders; this headroom is entirely for the
  // replayed venue book.
  std::uint32_t arena_capacity = 1U << 17;
  std::uint64_t sharpe_sample_interval_ns = 1000ULL * 1000ULL * 1000ULL;

  // Nanoseconds between the strategy deciding and the venue acting on it, applied
  // to placements and cancels alike. Zero is the idealised bound rather than a
  // realistic setting; see reconcile_side for why it is the largest single
  // optimism a backtest can carry, and STRATEGY.md for what it costs measured.
  std::uint64_t latency_ns = 0;
};

// Wires the replayed venue book, the strategy, the queue estimator, and the
// accounting into one pass over an ITCH stream.
//
// The engine and the replay driver deliberately share one book. Replay applies
// venue messages to it directly, without matching, because an execution message
// reports a trade that already happened. Strategy orders go through the engine,
// because those are hypothetical and have to contend for queue position against
// the reconstructed book. Both facts are recorded in DESIGN.md, "Replay
// reconstructs; it does not re-match".
template <std::size_t BandLevels = DEFAULT_BAND_LEVELS>
class Backtest {
 public:
  using EngineType = Engine<CancelNewest, BandLevels>;
  using BookType = typename EngineType::BookType;

  explicit Backtest(const BacktestConfig& config)
      : config_(config),
        engine_(make_config(config)),
        driver_(engine_.book(), config.symbol),
        maker_(config.maker),
        pnl_(config.fees) {}

  [[nodiscard]] const PnlAccount& pnl() const noexcept { return pnl_; }

  [[nodiscard]] const CrossedBookStats& crossed() const noexcept { return crossed_; }

  [[nodiscard]] const itch::ReplayStats& replay_stats() const noexcept { return driver_.stats(); }

  [[nodiscard]] std::uint64_t quotes_placed() const noexcept { return quotes_placed_; }

  [[nodiscard]] std::uint64_t quotes_cancelled() const noexcept { return quotes_cancelled_; }

  [[nodiscard]] std::uint64_t quotes_rejected() const noexcept { return quotes_rejected_; }

  // Cancels that arrived after their target had already filled. Zero without
  // latency; with it, the count of times the strategy was too slow to pull.
  [[nodiscard]] std::uint64_t cancels_too_late() const noexcept { return cancels_too_late_; }

  // Largest excursion beyond the configured position limit, in shares. Non-zero
  // only when an in-flight quote landed and filled after the position had already
  // reached the limit. See reconcile_side for why the race is kept.
  [[nodiscard]] std::int64_t limit_overshoot_shares() const noexcept {
    return limit_overshoot_shares_;
  }

  void run(std::span<const std::byte> data) {
    static_cast<void>(itch::for_each_message(
        data, [this](const itch::MessageView& message) { on_message(message); }));
    finalise();
  }

 private:
  // A sink that discards. The strategy learns about its own fills from the queue
  // estimator rather than from engine events, because a fill here is inferred
  // from what the venue did, not produced by matching.
  struct NullSink {
    void push(const ExecutionEvent& /*event*/) const noexcept {}
  };

  static typename EngineType::Config make_config(const BacktestConfig& config) {
    typename EngineType::Config engine_config;
    engine_config.price = PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 0};
    engine_config.arena_capacity = config.arena_capacity;
    engine_config.initial_center = Ticks{0};
    return engine_config;
  }

  // The order reference sits at offset 11 on every message that names one,
  // including the original reference on a replace.
  [[nodiscard]] static std::optional<OrderId> referenced_order(const itch::MessageView& message) {
    switch (message.type()) {
      case itch::MessageType::order_executed:
      case itch::MessageType::order_executed_with_price:
      case itch::MessageType::order_cancel:
      case itch::MessageType::order_delete:
      case itch::MessageType::order_replace:
        return OrderId{message.u64_at(itch::order_executed::ORDER_REFERENCE)};
      default:
        return std::nullopt;
    }
  }

  // One decision waiting out its latency window.
  struct PendingAction {
    std::uint64_t effective_ns = 0;
    enum class Kind : std::uint8_t { place, cancel };
    Kind kind = Kind::place;
    Side side = Side::buy;
    Ticks price{};
    OrderId id{};
  };

  struct OrderSnapshot {
    bool present = false;
    Side side = Side::buy;
    Ticks price{};
    std::uint64_t remaining = 0;
  };

  [[nodiscard]] OrderSnapshot snapshot(OrderId id) const {
    OrderSnapshot snap;
    const ArenaIndex index = engine_.book().find_order(id);
    if (index == INVALID_INDEX) {
      return snap;
    }
    const Order& order = engine_.book().pool()[index];
    snap.present = true;
    snap.side = order.side;
    snap.price = order.price;
    snap.remaining = order.remaining;
    return snap;
  }

  // The venue's best price, excluding the strategy's own resting orders.
  //
  // Needed because the strategy's quotes live in the same book. Marking to a mid
  // the strategy itself set, or quoting around it, is a feedback loop that makes
  // a backtest drift away from the market it is supposed to be replaying.
  [[nodiscard]] std::optional<Ticks> venue_best(Side side) const {
    std::optional<Ticks> price =
        side == Side::buy ? engine_.book().best_bid() : engine_.book().best_ask();

    while (price.has_value()) {
      const std::uint64_t total = engine_.book().total_qty_at(side, *price).raw();
      const std::uint64_t ours = our_quantity_at(side, *price);
      if (total > ours) {
        return price;
      }
      price = engine_.book().next_level_away(side, *price);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::uint64_t our_quantity_at(Side side, Ticks price) const {
    std::uint64_t total = 0;
    for (const StrategyOrder& order : queue_.orders()) {
      if (order.side == side && order.price == price) {
        total += order.remaining;
      }
    }
    return total;
  }

  void on_message(const itch::MessageView& message) {
    const std::uint64_t timestamp = message.timestamp();

    // Anything the strategy decided earlier whose latency window has now elapsed
    // reaches the book before this message does.
    flush_pending(timestamp);

    const std::optional<OrderId> reference = referenced_order(message);
    OrderSnapshot before;
    if (reference.has_value()) {
      before = snapshot(*reference);
    }

    driver_.apply(message);

    if (reference.has_value() && before.present) {
      const OrderSnapshot after = snapshot(*reference);
      const std::uint64_t remaining_after = after.present ? after.remaining : 0;
      if (before.remaining > remaining_after) {
        VenueEvent event;
        event.side = before.side;
        event.price = before.price;
        event.quantity = before.remaining - remaining_after;
        event.timestamp_ns = timestamp;
        event.kind =
            is_execution(message.type()) ? VenueEvent::Kind::execution : VenueEvent::Kind::cancel;
        apply_venue_event(event);
      }
    }

    queue_.refresh_behind(engine_.book());

    const std::optional<Ticks> venue_bid = venue_best(Side::buy);
    const std::optional<Ticks> venue_ask = venue_best(Side::sell);

    if (venue_bid.has_value() && venue_ask.has_value()) {
      const double mid =
          (static_cast<double>(venue_bid->raw()) + static_cast<double>(venue_ask->raw())) / 2.0;
      pnl_.mark(mid, timestamp);
      pnl_.sample_if_due(timestamp, config_.sharpe_sample_interval_ns);
      observe_crossed(timestamp);
      requote(venue_bid, venue_ask, mid, timestamp);
    }
  }

  [[nodiscard]] static bool is_execution(itch::MessageType type) noexcept {
    return type == itch::MessageType::order_executed ||
           type == itch::MessageType::order_executed_with_price;
  }

  void apply_venue_event(const VenueEvent& event) {
    const std::vector<Fill> fills = queue_.on_venue_event(event);
    for (const Fill& fill : fills) {
      // Every fill here is passive: the strategy was resting and the market came
      // to it. Post-only submission means there is no taker path.
      pnl_.on_fill(fill, true);
      remove_filled_quantity(fill);
    }
    if (!fills.empty()) {
      queue_.purge_filled();
      observe_limit_overshoot();
    }
  }

  // Records any excursion past the configured position limit. See reconcile_side:
  // the limit is a decision-time check, so an in-flight quote can land and fill
  // after the position has already reached it.
  void observe_limit_overshoot() {
    const std::int64_t limit = config_.maker.position_limit;
    const std::int64_t position = pnl_.position();
    std::int64_t excess = 0;
    if (position > limit) {
      excess = position - limit;
    } else if (position < -limit) {
      excess = -limit - position;
    }

    if (excess > limit_overshoot_shares_) {
      limit_overshoot_shares_ = excess;
    }
  }

  // The strategy's order is a real order in a real book, so a modelled fill has
  // to take the shares out of it. Leaving it resting would keep phantom liquidity
  // in the reconstructed book for the rest of the run.
  void remove_filled_quantity(const Fill& fill) {
    const ArenaIndex index = engine_.book().find_order(fill.id);
    if (index == INVALID_INDEX) {
      return;
    }
    const std::uint64_t remaining = engine_.book().pool()[index].remaining;
    if (fill.quantity >= remaining) {
      engine_.book().remove_by_index(index);
      return;
    }
    engine_.book().reduce_by_index(index, Quantity{fill.quantity});
  }

  // Crossed means the bid is strictly above the ask. Bid equal to ask is a
  // **locked** book, which is a different state and is counted separately.
  //
  // The distinction matters rather than being pedantry. A locked book is legal on
  // several venues and arises naturally when the strategy joins the far side at
  // the touch, whereas a crossed book cannot exist at a real venue at all. Folding
  // locked into crossed inflated the statistic that exists specifically to bound
  // how much the fill model's honesty is worth, which is the one number that
  // should not be inflated.
  void observe_crossed(std::uint64_t timestamp) {
    ++crossed_.observations;

    const std::optional<Ticks> best_bid = engine_.book().best_bid();
    const std::optional<Ticks> best_ask = engine_.book().best_ask();
    const bool two_sided = best_bid.has_value() && best_ask.has_value();

    const bool crossed = two_sided && best_bid->raw() > best_ask->raw();
    const bool locked = two_sided && best_bid->raw() == best_ask->raw();

    if (locked) {
      ++crossed_.locked_observations;
    }

    if (crossed) {
      ++crossed_.crossed_observations;
      const std::int32_t depth = best_bid->raw() - best_ask->raw();
      if (depth > crossed_.worst_depth_ticks) {
        crossed_.worst_depth_ticks = depth;
      }
      if (!was_crossed_) {
        ++crossed_.intervals;
        crossed_since_ = timestamp;
      }
    } else if (was_crossed_ && timestamp > crossed_since_) {
      crossed_.total_ns += timestamp - crossed_since_;
    }

    was_crossed_ = crossed;
    last_timestamp_ = timestamp;
  }

  // Closes an interval still open when the stream ends.
  //
  // Without this a replay that finishes crossed contributes its interval to the
  // count but nothing to the duration, so the two figures disagree and the
  // duration is quietly short. Rare, but it is the kind of end-of-stream omission
  // that is invisible until someone reconciles the two columns.
  void finalise() {
    if (was_crossed_ && last_timestamp_ > crossed_since_) {
      crossed_.total_ns += last_timestamp_ - crossed_since_;
      was_crossed_ = false;
    }
  }

  // The requote trigger, and it matters more than it looks.
  //
  // A first version reconciled on every book update. The result was 53 873 quotes
  // placed against 37 fills, a fill ratio of 0.0006, because every requote goes to
  // the back of its queue and the next message cancelled it before it could ever
  // reach the front. Constantly resetting queue position is the most expensive
  // thing a market maker can do to itself, and the measurement said so.
  //
  // So quotes are left alone unless the mid has moved at least
  // `requote_mid_move_ticks`, or a side that should be quoted has nothing resting
  // because it filled or was cancelled. That makes the parameter real rather than
  // declared, and it is one of the axes the sweep varies.
  void requote(std::optional<Ticks> venue_bid,
               std::optional<Ticks> venue_ask,
               double mid,
               std::uint64_t timestamp) {
    const QuoteIntent intent = maker_.desired(venue_bid, venue_ask, pnl_.position());

    const auto threshold = static_cast<double>(config_.maker.requote_mid_move_ticks);
    const bool mid_moved = !has_quote_mid_ || std::fabs(mid - quote_mid_) >= threshold;

    const bool bid_missing = intent.want_bid && !resting_on(Side::buy).has_value();
    const bool ask_missing = intent.want_ask && !resting_on(Side::sell).has_value();

    // A resting quote on a side the strategy no longer wants must go regardless
    // of the mid, because that is the inventory limit taking effect.
    const bool unwanted_bid = !intent.want_bid && resting_on(Side::buy).has_value();
    const bool unwanted_ask = !intent.want_ask && resting_on(Side::sell).has_value();

    if (!mid_moved && !bid_missing && !ask_missing && !unwanted_bid && !unwanted_ask) {
      return;
    }

    reconcile_side(Side::buy, intent.want_bid, intent.bid_price, timestamp);
    reconcile_side(Side::sell, intent.want_ask, intent.ask_price, timestamp);

    quote_mid_ = mid;
    has_quote_mid_ = true;
  }

  // Cancel and replace only when the target price actually moved. Requoting on
  // every message would produce a cancel rate no venue would tolerate and would
  // reset queue position constantly, which is the single most expensive thing a
  // market maker can do to itself.
  // Reaction latency, and why it is the most important thing this backtest models
  // that a naive one does not.
  //
  // A decision made from the book as it stands at time T cannot take effect at T.
  // A real participant sees the message later, decides later, and its order
  // reaches the matching engine later still. Everything that happens in between
  // happens without the order being there, and every order that arrived during the
  // gap is ahead of it in the queue.
  //
  // Modelling it as zero was the single largest unmodelled optimism in Phase 5,
  // larger than the uniform cancel assumption that gets far more discussion,
  // because it flatters queue position on every single quote rather than
  // marginally adjusting one estimate.
  //
  // Cancels are delayed too, and that half turned out to dominate. A quote the
  // strategy has decided to pull is still resting, and still fillable, for the
  // whole window. Delaying only the placement would model a participant that can
  // retract instantly but not act instantly, which is backwards.
  //
  // **Two races come with this, and both are kept rather than engineered away,
  // because a real participant has both.**
  //
  // The position limit is a decision-time check. `MarketMaker::desired` tests it
  // against exposure as it stands when the quote is decided, so a bid decided at
  // position 900 can land after a fill has already moved the position to 1 000,
  // rest, fill, and settle at 1 100. Re-testing at landing was the alternative and
  // was rejected: a venue does not re-underwrite an order that is already in
  // flight, and modelling a control the strategy could not actually have would
  // flatter it. The overshoot is measured instead and reported as
  // `limit_overshoot_shares`, so the cost of the race is visible rather than
  // assumed to be zero.
  //
  // An in-flight placement also carries the price decided a window earlier, even
  // if the desired price has moved since. It is not re-decided at landing, which
  // models an order that cannot be amended while unacknowledged. Post-only turns
  // the worst case into a rejection rather than a cross, and the in-flight flag
  // stops a corrected duplicate queueing behind it.
  //
  // The measured effect is not the one predicted before running it, which is
  // worth recording. The expectation was that fills would fall as latency grew,
  // because orders arriving during the window queue ahead of ours. Fills instead
  // **rise**, 38 at zero latency to 53 at a millisecond, because the delayed
  // cancel leaves the quote exposed for longer and that outweighs the queue
  // position lost. What degrades is fill quality: the 1 s markout falls from 0.85
  // to roughly 0.5 per share over the same range. That is adverse selection
  // appearing where theory says it should, since the trades a participant cannot
  // pull away from are disproportionately the ones it would most want to.
  void reconcile_side(Side side, bool want, Ticks price, std::uint64_t timestamp) {
    const auto index = static_cast<std::size_t>(side);
    const std::optional<StrategyOrder> resting = resting_on(side);

    if (resting.has_value()) {
      if (want && resting->price == price) {
        return;
      }
      if (!cancel_in_flight_[index]) {
        schedule(PendingAction{.effective_ns = timestamp + config_.latency_ns,
                               .kind = PendingAction::Kind::cancel,
                               .side = side,
                               .price = Ticks{},
                               .id = resting->id});
      }
      // The replacement is scheduled below rather than skipped, so it lands
      // behind its own cancel in the queue, which is the real ordering.
    }

    if (!want || place_in_flight_[index]) {
      return;
    }

    schedule(PendingAction{.effective_ns = timestamp + config_.latency_ns,
                           .kind = PendingAction::Kind::place,
                           .side = side,
                           .price = price,
                           .id = OrderId{}});
  }

  // Owns the in-flight flags outright, so that a caller cannot set one and then
  // have the immediate path clear it, which is precisely the bug the first
  // version of this had: the flags stuck true after the first quote and the
  // strategy never traded again.
  void schedule(const PendingAction& action) {
    if (config_.latency_ns == 0) {
      // Zero latency is the idealised bound rather than a realistic setting, and
      // it is kept exactly reachable so the cost of latency reads off as a
      // difference against it. Nothing is ever queued, so the deferred path
      // cannot perturb the baseline.
      apply(action, action.effective_ns);
      return;
    }
    set_in_flight(action, true);
    pending_.push_back(action);
  }

  void set_in_flight(const PendingAction& action, bool value) {
    const auto index = static_cast<std::size_t>(action.side);
    if (action.kind == PendingAction::Kind::cancel) {
      cancel_in_flight_[index] = value;
    } else {
      place_in_flight_[index] = value;
    }
  }

  // Applies every action whose latency window has elapsed by `now_ns`. Called
  // before the message at that timestamp is applied, so the strategy's order
  // reaches the book just ahead of it, which is the closest this can get to the
  // order arriving during the gap.
  void flush_pending(std::uint64_t now_ns) {
    while (!pending_.empty() && pending_.front().effective_ns <= now_ns) {
      const PendingAction action = pending_.front();
      pending_.pop_front();
      set_in_flight(action, false);
      apply(action, now_ns);
    }
  }

  void apply(const PendingAction& action, std::uint64_t now_ns) {
    if (action.kind == PendingAction::Kind::cancel) {
      cancel(action.id);
      return;
    }
    // The order may have been filled or pulled while this was in flight, in
    // which case placing a second one on the same side would double the
    // strategy's exposure.
    if (!resting_on(action.side).has_value()) {
      place(action.side, action.price, now_ns);
    }
  }

  [[nodiscard]] std::optional<StrategyOrder> resting_on(Side side) const {
    for (const StrategyOrder& order : queue_.orders()) {
      if (order.side == side && order.remaining > 0) {
        return order;
      }
    }
    return std::nullopt;
  }

  // A cancel that lands after its target has already filled is counted
  // separately rather than as a cancel.
  //
  // With latency on it is not an error, it is the measurement: it counts the times
  // the strategy decided to pull a quote and the market got there first. Counting
  // it as a cancel would also stop the placed, cancelled and rejected totals
  // reconciling against fills, which is how the discrepancy was noticed.
  void cancel(OrderId id) {
    const bool still_resting = engine_.book().find_order(id) != INVALID_INDEX;

    Command command;
    command.id = id;
    command.type = CommandType::cancel;
    command.party = STRATEGY_PARTICIPANT;

    NullSink sink;
    engine_.submit(command, sink);
    queue_.on_cancel(id);

    if (still_resting) {
      ++quotes_cancelled_;
    } else {
      ++cancels_too_late_;
    }
  }

  void place(Side side, Ticks price, std::uint64_t timestamp) {
    const std::uint64_t ahead = engine_.book().total_qty_at(side, price).raw();

    Command command;
    command.id = OrderId{next_order_id_};
    command.type = CommandType::add;
    command.order_type = OrderType::post_only;
    command.side = side;
    command.price = engine_.book().price_config().to_price(price);
    command.quantity = Quantity{config_.maker.quote_size};
    command.timestamp = Timestamp{timestamp};
    command.party = STRATEGY_PARTICIPANT;

    NullSink sink;
    engine_.submit(command, sink);

    const ArenaIndex index = engine_.book().find_order(OrderId{next_order_id_});
    if (index == INVALID_INDEX) {
      // Rejected, which for a post-only quote means it would have crossed. Not
      // an error: it is the strategy declining to pay the spread.
      ++quotes_rejected_;
      ++next_order_id_;
      return;
    }

    StrategyOrder order;
    order.id = OrderId{next_order_id_};
    order.side = side;
    order.price = price;
    order.remaining = config_.maker.quote_size;
    queue_.on_insert(order, ahead);

    pnl_.on_quote_submitted(config_.maker.quote_size);
    ++quotes_placed_;
    ++next_order_id_;
  }

  BacktestConfig config_;
  EngineType engine_;
  itch::ReplayDriver<BandLevels> driver_;
  MarketMaker maker_;
  QueueEstimator queue_;
  PnlAccount pnl_;
  CrossedBookStats crossed_;

  // Strategy ids start high enough that they cannot collide with a venue order
  // reference from the capture, which are assigned by NASDAQ from a separate
  // space and are not bounded below this.
  std::uint64_t next_order_id_ = 1ULL << 62U;

  std::deque<PendingAction> pending_;
  std::array<bool, 2> place_in_flight_{};
  std::array<bool, 2> cancel_in_flight_{};

  std::uint64_t quotes_placed_ = 0;
  std::uint64_t quotes_cancelled_ = 0;
  std::uint64_t quotes_rejected_ = 0;
  std::uint64_t cancels_too_late_ = 0;
  std::int64_t limit_overshoot_shares_ = 0;

  // Timestamp of the most recent message, needed only so `finalise` can close a
  // crossed interval that is still open when the stream ends. It was removed once
  // as dead code, correctly at the time, and is back because that end-of-stream
  // case turned out to be a real omission rather than an unused field.
  std::uint64_t last_timestamp_ = 0;

  std::uint64_t crossed_since_ = 0;
  bool was_crossed_ = false;

  double quote_mid_ = 0.0;
  bool has_quote_mid_ = false;
};

}  // namespace ob::strategy
