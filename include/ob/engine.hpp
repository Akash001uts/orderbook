#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "ob/book.hpp"
#include "ob/events.hpp"
#include "ob/order.hpp"
#include "ob/types.hpp"

namespace ob {

// The event sink is full and the engine has an event it cannot deliver. There is
// no recoverable choice here: dropping the event would desynchronise any consumer
// reconstructing state from the stream, and the engine cannot resize a sink it does
// not own. submit is noexcept, so this cannot throw either. It aborts, in debug and
// release alike, rather than silently losing an event. The caller's contract is to
// size the sink for the most events one command can produce; see Engine::submit.
[[noreturn]] inline void fatal_event_sink_overflow() noexcept {
  std::fputs(
      "ob::Engine: the event sink rejected a push because it is full. A sink must "
      "hold every event a single command can emit, which for a marketable order is "
      "one fill per resting order it consumes plus a terminal event. Aborting rather "
      "than dropping an event.\n",
      stderr);
  std::abort();
}

// What to do when an incoming order would trade against a resting order entered by
// the same participant.
enum class StpAction : std::uint8_t {
  allow,             // no self trade, proceed with the match
  cancel_aggressor,  // remove the incoming order's remainder, leave the resting one
  cancel_resting,    // remove the resting order, let the aggressor continue
};

// Self trade prevention policies.
//
// The policy is a template parameter rather than a runtime flag because it is
// consulted on every single fill, and a runtime branch there would be an indirect
// call or an unpredictable test in the hottest loop the engine has. As a template
// parameter it is a compile time constant, so the whole check folds away for the
// overwhelmingly common case where the two participants differ.
//
// Two policies exist rather than one deliberately: a template parameter with a
// single instantiation is the appearance of extensibility rather than the thing
// itself. Which further policies were left out, and why, is in DESIGN.md, "Self
// trade prevention as a template parameter".
//
// Both policies short circuit on a zero aggressor, which is load bearing rather
// than an optimisation. Replayed venue orders all carry NO_PARTICIPANT, so without
// it every venue order would count as self trading with every other one.

// The venue default here and at NASDAQ. The incoming order is by definition the
// newer of the two, so it is the one that gives way, and a resting order never
// loses its place because somebody else's aggressor arrived.
struct CancelNewest {
  [[nodiscard]] static constexpr StpAction resolve(ParticipantId aggressor,
                                                   ParticipantId resting) noexcept {
    if (aggressor == NO_PARTICIPANT || aggressor != resting) {
      return StpAction::allow;
    }
    return StpAction::cancel_aggressor;
  }
};

// The mirror image: the resting order is removed and the aggressor carries on into
// the rest of the book. Useful when a participant would rather refresh stale
// quotes than have new interest blocked by them.
struct CancelOldest {
  [[nodiscard]] static constexpr StpAction resolve(ParticipantId aggressor,
                                                   ParticipantId resting) noexcept {
    if (aggressor == NO_PARTICIPANT || aggressor != resting) {
      return StpAction::allow;
    }
    return StpAction::cancel_resting;
  }
};

// Matching engine over a single book.
//
// Determinism is the property everything else rests on. The engine reads no clock,
// consumes no randomness, performs no I/O, and allocates nothing on the in-band
// path. Time arrives as a field on the command. The same command sequence
// therefore produces byte identical book state and a byte identical event stream
// on every run, which is what makes the differential test in test_differential.cpp
// a proof rather than a smoke test.
template <typename StpPolicy = CancelNewest, std::size_t BandLevels = DEFAULT_BAND_LEVELS>
class Engine {
 public:
  using BookType = Book<BandLevels>;
  using Config = BookType::Config;

  // book_(config) validates the configuration first. The representable-price bounds
  // are then derived once here, at the cold construction boundary, so the per-command
  // path checks two comparisons rather than repeating a division on every add.
  explicit Engine(const Config& config)
      : book_(config),
        min_price_raw_(config.price.min_representable_raw()),
        max_price_raw_(config.price.max_representable_raw()) {}

  [[nodiscard]] const BookType& book() const noexcept { return book_; }

  [[nodiscard]] BookType& book() noexcept { return book_; }

  [[nodiscard]] Sequence next_sequence() const noexcept { return next_sequence_; }

  // Submits one command and appends the events it produced to the sink.
  //
  // Sink is a template parameter so the append inlines. A std::function or a
  // virtual sink would put an indirect call on every fill, which the compiler
  // could neither inline nor optimise across. A Sink must expose
  // `bool push(const ExecutionEvent&)`, returning false only when it is full and
  // true otherwise. A sink that cannot reject (an unbounded or discarding one)
  // returns true unconditionally, and the overflow branch folds away for it.
  //
  // Source-compatibility note: push returns bool. It was previously void, so a
  // custom sink that declared `void push(const ExecutionEvent&)` no longer
  // compiles and must be updated to return bool. This is deliberate: emit reads the
  // result to enforce the overflow policy, and a void push gave it nothing to read.
  //
  // Capacity is the caller's responsibility, and one command can emit many events:
  //   - a marketable order emits a fill per resting order it consumes, across as
  //     many price levels as it sweeps, plus a terminal cancel or book update;
  //   - self-trade prevention can emit a cancel per resting order it removes;
  //   - a modify emits a book update, and a resting add emits a book update.
  // Size the sink for the largest such burst, not for one event per level. If the
  // same sink accumulates across several commands without being drained, size it
  // for the sum. This call is noexcept and does not roll back: a sink that rejects
  // a push is a fatal sizing error and aborts the process (in debug and release
  // alike) rather than dropping an event, continuing, or unwinding a half-applied
  // command. Drain the sink between commands so it starts each command empty.
  template <typename Sink>
  void submit(const Command& command, Sink& sink) noexcept {
    switch (command.type) {
      case CommandType::add:
        submit_add(command, sink);
        return;
      case CommandType::cancel:
        submit_cancel(command, sink);
        return;
      case CommandType::modify:
        submit_modify(command, sink);
        return;
    }
  }

 private:
  // -------------------------------------------------------------------------
  // Event emission. Every event gets a sequence number from a counter the engine
  // owns, so ordering is total and reproducible without consulting a clock.
  // -------------------------------------------------------------------------

  template <typename Sink>
  void emit(Sink& sink, const ExecutionEvent& event) noexcept {
    ExecutionEvent stamped = event;
    stamped.sequence = next_sequence_;
    next_sequence_ += Sequence{1};
    // A sink reports a rejected push by returning false. That is unrecoverable
    // here, so the policy is to terminate rather than drop the event or unwind a
    // half-applied command. A sink that cannot reject (a discarding one) returns
    // true unconditionally, so this branch folds away for it.
    if (!sink.push(stamped)) [[unlikely]] {
      fatal_event_sink_overflow();
    }
  }

  template <typename Sink>
  void emit_reject(Sink& sink, const Command& command, RejectReason reason) noexcept {
    ExecutionEvent event;
    event.type = EventType::rejected;
    event.reason = reason;
    event.taker_id = command.id;
    event.quantity = command.quantity;
    event.timestamp = command.timestamp;
    event.aggressor_side = command.side;
    emit(sink, event);
  }

  template <typename Sink>
  void emit_cancel(Sink& sink,
                   OrderId id,
                   Ticks price,
                   Quantity quantity,
                   Timestamp timestamp,
                   Side side,
                   RejectReason reason) noexcept {
    ExecutionEvent event;
    event.type = EventType::cancelled;
    event.reason = reason;
    event.taker_id = id;
    event.price = price;
    event.quantity = quantity;
    event.timestamp = timestamp;
    event.aggressor_side = side;
    emit(sink, event);
  }

  template <typename Sink>
  void emit_rested(Sink& sink, const Command& command, Ticks price, Quantity quantity) noexcept {
    ExecutionEvent event;
    event.type = EventType::book_update;
    event.taker_id = command.id;
    event.price = price;
    event.quantity = quantity;
    event.timestamp = command.timestamp;
    event.aggressor_side = command.side;
    emit(sink, event);
  }

  // -------------------------------------------------------------------------
  // Add, which is where all the order type semantics live.
  // -------------------------------------------------------------------------

  template <typename Sink>
  void submit_add(const Command& command, Sink& sink) noexcept {
    if (command.id.raw() == 0U || book_.find_order(command.id) != INVALID_INDEX) {
      emit_reject(sink, command, RejectReason::duplicate_order);
      return;
    }
    if (command.quantity.raw() == 0U) {
      emit_reject(sink, command, RejectReason::zero_quantity);
      return;
    }
    if (command.quantity.raw() > MAX_ORDER_SHARES) {
      emit_reject(sink, command, RejectReason::quantity_too_large);
      return;
    }

    const PriceConfig& price_config = book_.price_config();

    // A market order has no price of its own. It is represented internally as a
    // limit at the most aggressive price expressible, so that one matching loop
    // serves every order type rather than there being a separate market path to
    // keep in agreement with the limit path.
    Ticks limit{};
    if (command.order_type == OrderType::market) {
      limit = command.side == Side::buy ? Ticks{MOST_AGGRESSIVE_BUY} : Ticks{MOST_AGGRESSIVE_SELL};
    } else {
      // A price outside the representable domain cannot be converted to a tick
      // without signed overflow, and no band or cold level could hold it, so reject
      // it before on_tick_boundary forms the subtraction that would overflow. The
      // bounds were derived once at construction, so this is two comparisons rather
      // than the division PriceConfig::representable performs.
      const std::int64_t raw = command.price.raw();
      if (raw < min_price_raw_ || raw > max_price_raw_) {
        emit_reject(sink, command, RejectReason::band_overflow);
        return;
      }
      if (!price_config.on_tick_boundary(command.price)) {
        emit_reject(sink, command, RejectReason::off_tick);
        return;
      }
      limit = price_config.to_ticks(command.price);
    }

    // Post-only must not trade. Checking before any mutation is the whole point:
    // a post-only that crosses is rejected outright, never silently repriced,
    // because a participant who asked to be a maker did not consent to paying the
    // spread.
    if (command.order_type == OrderType::post_only && crosses(command.side, limit)) {
      emit_reject(sink, command, RejectReason::would_cross);
      return;
    }

    // Fill-or-kill totals the reachable liquidity before touching anything. This
    // is the one order type where a partial mutation would be observably wrong,
    // so the check is a genuine precondition rather than an optimisation.
    if (command.order_type == OrderType::fill_or_kill &&
        !has_liquidity_for(command.side, limit, command.quantity, command.party)) {
      emit_reject(sink, command, RejectReason::insufficient_liquidity);
      return;
    }

    // A market order has no price, so its events report zero rather than the
    // synthetic limit above. Leaking an internal sentinel into the event stream
    // would make a consumer's price field meaningless for exactly the order type
    // where a reader is most likely to look at it.
    const Ticks event_price = command.order_type == OrderType::market ? Ticks{0} : limit;

    Quantity remaining = command.quantity;
    const bool aggressor_cancelled = match(command, limit, remaining, sink);

    if (remaining.raw() == 0U) {
      return;
    }

    // Self trade prevention removed the aggressor's remainder. It does not rest.
    if (aggressor_cancelled) {
      emit_cancel(sink,
                  command.id,
                  event_price,
                  remaining,
                  command.timestamp,
                  command.side,
                  RejectReason::self_trade);
      return;
    }

    // Market, immediate-or-cancel, and fill-or-kill never rest. Fill-or-kill can
    // only reach here having been fully filled, since the precondition above
    // guaranteed the liquidity, so its remainder is always zero.
    if (command.order_type != OrderType::limit && command.order_type != OrderType::post_only) {
      assert(command.order_type != OrderType::fill_or_kill &&
             "fill-or-kill passed its liquidity check and still has a remainder");
      emit_cancel(sink,
                  command.id,
                  event_price,
                  remaining,
                  command.timestamp,
                  command.side,
                  RejectReason::none);
      return;
    }

    rest(command, limit, remaining, sink);
  }

  template <typename Sink>
  void rest(const Command& command, Ticks limit, Quantity remaining, Sink& sink) noexcept {
    AddRequest request;
    request.id = command.id;
    request.price = limit;
    request.quantity = remaining;
    request.timestamp = command.timestamp;
    request.party = command.party;
    request.side = command.side;

    // This is the only add in the engine, and it happens strictly after the match
    // loop has finished. That ordering is what keeps a band rebase out of the
    // middle of a match; Book::LevelWalk asserts it.
    const AddStatus status = book_.add(request);
    if (status == AddStatus::ok) {
      emit_rested(sink, command, limit, remaining);
      return;
    }

    emit_reject(sink, command, reject_reason_for(status));
  }

  [[nodiscard]] static RejectReason reject_reason_for(AddStatus status) noexcept {
    switch (status) {
      case AddStatus::duplicate_id:
        return RejectReason::duplicate_order;
      case AddStatus::zero_quantity:
        return RejectReason::zero_quantity;
      case AddStatus::quantity_too_large:
        return RejectReason::quantity_too_large;
      case AddStatus::arena_exhausted:
      case AddStatus::map_exhausted:
        return RejectReason::arena_exhausted;
      case AddStatus::band_overflow:
        return RejectReason::band_overflow;
      case AddStatus::invalid_id:
        return RejectReason::unknown_order;
      case AddStatus::ok:
        break;
    }
    assert(false && "reject_reason_for called on a successful add");
    return RejectReason::none;
  }

  // -------------------------------------------------------------------------
  // The match loop.
  //
  // Returns true when self trade prevention cancelled the aggressor's remainder.
  // remaining is updated in place.
  //
  // The loop walks levels from the best opposing price outward, consuming whole
  // orders and partially filling the last. It never searches for the next level:
  // consuming a level clears its occupancy bit, so the following best_* query
  // returns the next price in constant time.
  // -------------------------------------------------------------------------

  template <typename Sink>
  [[nodiscard]] bool match(const Command& command,
                           Ticks limit,
                           Quantity& remaining,
                           Sink& sink) noexcept {
    const Side opposing = opposite(command.side);
    const typename BookType::LevelWalk walk(book_);

    while (remaining.raw() > 0U) {
      const std::optional<Ticks> best = opposing == Side::buy ? book_.best_bid() : book_.best_ask();
      if (!best.has_value() || !price_crosses(command.side, limit, *best)) {
        return false;
      }

      ArenaIndex index = book_.first_order_at(opposing, *best);
      if (index == INVALID_INDEX) {
        // The occupancy bitmap claimed a level that holds no orders. That is the
        // one inconsistency this design can produce, so it is checked rather than
        // trusted, and bailing out is better than looping forever on it.
        assert(false && "occupancy bitmap disagrees with the level it points at");
        return false;
      }

      while (index != INVALID_INDEX && remaining.raw() > 0U) {
        const ArenaIndex current = index;
        const Order& resting = book_.pool()[current];
        index = resting.next;

        const StpAction action = StpPolicy::resolve(command.party, resting.party);
        if (action == StpAction::cancel_aggressor) {
          return true;
        }
        if (action == StpAction::cancel_resting) {
          const OrderId resting_id = resting.id;
          const Quantity resting_qty = resting.remaining_qty();
          const Timestamp resting_ts = resting.timestamp;
          book_.remove_by_index(current);
          emit_cancel(
              sink, resting_id, *best, resting_qty, resting_ts, opposing, RejectReason::self_trade);
          continue;
        }

        const Quantity available = resting.remaining_qty();
        const Quantity traded = available.raw() <= remaining.raw() ? available : remaining;

        ExecutionEvent fill;
        fill.taker_id = command.id;
        fill.maker_id = resting.id;
        fill.quantity = traded;
        fill.remaining = Quantity{available.raw() - traded.raw()};
        fill.timestamp = command.timestamp;
        fill.price = *best;
        fill.aggressor_side = command.side;
        fill.type = traded.raw() == available.raw() ? EventType::fill : EventType::partial_fill;

        if (traded.raw() == available.raw()) {
          book_.remove_by_index(current);
        } else {
          book_.reduce_by_index(current, traded);
        }

        remaining -= traded;
        emit(sink, fill);
      }

      // Liveness backstop. Every path through the inner loop either fills an
      // order, removes one, or returns, so arriving here with quantity left means
      // the level was emptied and the next best query will return a different
      // price. If that ever stopped holding, the outer loop would spin forever
      // rather than fail visibly, so it is checked rather than assumed.
      if (remaining.raw() > 0U && book_.first_order_at(opposing, *best) != INVALID_INDEX) {
        assert(false && "match made no progress at a level that still holds orders");
        return false;
      }
    }

    return false;
  }

  // -------------------------------------------------------------------------
  // Fill-or-kill's precondition.
  //
  // Totals the size reachable at acceptable prices without mutating anything.
  // Orders that self trade prevention would remove are excluded from the total,
  // because counting size the aggressor cannot legally trade against would let a
  // fill-or-kill pass its check and then fail to fill, which is the exact outcome
  // the check exists to prevent.
  // -------------------------------------------------------------------------

  [[nodiscard]] bool has_liquidity_for(Side side,
                                       Ticks limit,
                                       Quantity wanted,
                                       ParticipantId party) const noexcept {
    const Side opposing = opposite(side);

    std::optional<Ticks> level = opposing == Side::buy ? book_.best_bid() : book_.best_ask();
    std::uint64_t total = 0;

    while (level.has_value() && price_crosses(side, limit, *level)) {
      for (ArenaIndex index = book_.first_order_at(opposing, *level); index != INVALID_INDEX;) {
        const Order& resting = book_.pool()[index];
        index = resting.next;

        const StpAction action = StpPolicy::resolve(party, resting.party);
        if (action == StpAction::cancel_aggressor) {
          // The aggressor would stop dead here, so nothing beyond this point is
          // reachable and the total cannot grow further.
          return total >= wanted.raw();
        }
        if (action == StpAction::cancel_resting) {
          continue;
        }

        total += resting.remaining;
        if (total >= wanted.raw()) {
          return true;
        }
      }

      level = book_.next_level_away(opposing, *level);
    }

    return total >= wanted.raw();
  }

  // -------------------------------------------------------------------------
  // Cancel and modify.
  // -------------------------------------------------------------------------

  template <typename Sink>
  void submit_cancel(const Command& command, Sink& sink) noexcept {
    const ArenaIndex index = book_.find_order(command.id);
    if (index == INVALID_INDEX) {
      emit_reject(sink, command, RejectReason::unknown_order);
      return;
    }

    const Order& order = book_.pool()[index];
    const Ticks price = order.price;
    const Quantity quantity = order.remaining_qty();
    const Side side = order.side;

    const CancelStatus status = book_.cancel(command.id);
    assert(status == CancelStatus::ok);
    static_cast<void>(status);

    emit_cancel(sink, command.id, price, quantity, command.timestamp, side, RejectReason::none);
  }

  // A modify carries the new quantity only. A price change is not expressible as
  // a modify, by definition: the order belongs to a different level afterwards, so
  // it is a cancel plus an add and the caller issues both, which is also what
  // makes both appear in the event stream.
  template <typename Sink>
  void submit_modify(const Command& command, Sink& sink) noexcept {
    const ArenaIndex index = book_.find_order(command.id);
    if (index == INVALID_INDEX) {
      emit_reject(sink, command, RejectReason::unknown_order);
      return;
    }

    if (command.quantity.raw() == 0U) {
      emit_reject(sink, command, RejectReason::zero_quantity);
      return;
    }
    if (command.quantity.raw() > MAX_ORDER_SHARES) {
      emit_reject(sink, command, RejectReason::quantity_too_large);
      return;
    }

    const Ticks price = book_.pool()[index].price;
    const Side side = book_.pool()[index].side;

    const ModifyStatus status = book_.modify(command.id, command.quantity);
    switch (status) {
      case ModifyStatus::reduced_in_place:
      case ModifyStatus::requeued:
      case ModifyStatus::unchanged: {
        ExecutionEvent event;
        event.type = EventType::book_update;
        event.taker_id = command.id;
        event.price = price;
        event.quantity = command.quantity;
        event.timestamp = command.timestamp;
        event.aggressor_side = side;
        emit(sink, event);
        return;
      }
      case ModifyStatus::unknown_order:
        emit_reject(sink, command, RejectReason::unknown_order);
        return;
      case ModifyStatus::zero_quantity:
        emit_reject(sink, command, RejectReason::zero_quantity);
        return;
      case ModifyStatus::quantity_too_large:
        emit_reject(sink, command, RejectReason::quantity_too_large);
        return;
    }
  }

  // -------------------------------------------------------------------------
  // Crossing predicates.
  // -------------------------------------------------------------------------

  // A buy crosses an ask at or below its limit; a sell crosses a bid at or above.
  [[nodiscard]] static bool price_crosses(Side aggressor, Ticks limit, Ticks resting) noexcept {
    return aggressor == Side::buy ? resting <= limit : resting >= limit;
  }

  [[nodiscard]] bool crosses(Side side, Ticks limit) const noexcept {
    const std::optional<Ticks> best = side == Side::buy ? book_.best_ask() : book_.best_bid();
    return best.has_value() && price_crosses(side, limit, *best);
  }

  // Sentinels for a market order's synthetic limit. They are one step inside the
  // extremes of the tick type rather than at them, so that any arithmetic on a
  // market order's price cannot overflow.
  static constexpr std::int32_t MOST_AGGRESSIVE_BUY = 0x3FFFFFFF;
  static constexpr std::int32_t MOST_AGGRESSIVE_SELL = -0x3FFFFFFF;

  BookType book_;
  Sequence next_sequence_{1};
  // Precomputed at construction from the price config; see submit_add.
  std::int64_t min_price_raw_;
  std::int64_t max_price_raw_;
};

}  // namespace ob
