#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ob/order.hpp"
#include "ob/types.hpp"

namespace ob::strategy {

// The strategy's reserved participant id. See DESIGN.md, "The strategy's
// participant id": replayed venue orders all carry NO_PARTICIPANT, which makes
// self trade prevention inert until something in the book has a real id.
constexpr ParticipantId STRATEGY_PARTICIPANT = 1;

// What the strategy learns about the venue's book from one replayed message.
//
// Derived by diffing the referenced order around the message rather than by
// decoding each message type's fields a second time. The replay driver already
// knows how to apply every message correctly; re-deriving the same semantics here
// would be two implementations of one rule, and they would drift.
struct VenueEvent {
  enum class Kind : std::uint8_t {
    none,       // the message did not change a resting order
    execution,  // shares traded against a resting order, E or C
    cancel,     // shares removed without trading, X, D, or the removal half of U
  };

  Kind kind = Kind::none;
  Side side = Side::buy;
  Ticks price{};
  std::uint64_t quantity = 0;
  std::uint64_t timestamp_ns = 0;
};

// One resting strategy order, with its estimated place in the queue.
struct StrategyOrder {
  OrderId id{};
  Side side = Side::buy;
  Ticks price{};
  std::uint64_t remaining = 0;
  std::uint64_t original_size = 0;
  std::uint64_t submitted_ns = 0;

  // Estimated shares ahead of us in this level's queue.
  //
  // Held as a double because the cancel adjustment below is fractional and
  // rounding it at every step would accumulate a bias in one direction. The
  // fractional value is honest about what it is: an estimate, not a count.
  double ahead = 0.0;

  // Shares resting behind us, tracked so the uniform cancel assumption has a
  // denominator. Recomputed from the book rather than maintained incrementally,
  // because the book is authoritative and an incremental count would drift.
  double behind = 0.0;
};

// One fill of a strategy order.
struct Fill {
  OrderId id{};
  Side side = Side::buy;
  Ticks price{};
  std::uint64_t quantity = 0;
  std::uint64_t timestamp_ns = 0;

  // True when the fill came from the market trading at a price strictly worse
  // for the resting side than our own, which implies the market traded through
  // us. Recorded separately because it is the more inferential of the two fill
  // paths and a reader should be able to see how much of the P&L depends on it.
  bool from_trade_through = false;
};

// Queue position estimation and the fill model.
//
// **This is an estimate and it cannot be made exact from public market data.**
// The reason is structural rather than a limitation of this implementation.
// TotalView-ITCH reports every order's arrival, execution and cancellation, but a
// cancel message names an order reference, and to know whether that reference sat
// ahead of us or behind us we would need to have been tracking every individual
// order at the level since before we joined. That is knowable in principle from a
// full order-by-order feed, and this project does reconstruct it: the book holds
// the real orders in queue sequence.
//
// What is genuinely unknowable is different and worth being precise about. Our
// order is hypothetical. It never existed, so no venue ever assigned it a place in
// the real queue, and every order that arrived after our hypothetical insertion
// would in reality have queued behind a book that did not contain us. The
// counterfactual queue is not observable at any level of feed detail.
//
// So the estimator uses the standard assumption, which is the one most worth
// questioning: **cancels are uniformly distributed through the queue.** When C
// shares are cancelled at our price and we believe A shares rest ahead and B
// behind, we reduce our estimate of A by C * A / (A + B). The alternative
// assumptions are worth naming because the choice is not obvious:
//
//   - All cancels behind us. Maximally pessimistic, never improves our position,
//     and wrong in a way that always understates fills.
//   - All cancels ahead of us. Maximally optimistic, and exactly the sort of
//     assumption that manufactures P&L.
//   - Uniform. Wrong in a knowable direction, because real cancels skew toward
//     recently placed orders and therefore toward the back of the queue, which
//     means uniform is mildly optimistic. Recorded rather than corrected, since
//     correcting it would need a parameter fitted to the data being tested.
class QueueEstimator {
 public:
  // Called when a strategy order is accepted onto the book. `resting_ahead` is
  // the level's aggregate quantity immediately before insertion, which is exactly
  // the quantity that has time priority over us.
  void on_insert(const StrategyOrder& order, std::uint64_t resting_ahead) {
    StrategyOrder tracked = order;
    tracked.ahead = static_cast<double>(resting_ahead);
    tracked.behind = 0.0;
    orders_.push_back(tracked);
  }

  void on_cancel(OrderId id) {
    for (std::size_t index = 0; index < orders_.size(); ++index) {
      if (orders_[index].id == id) {
        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(index));
        return;
      }
    }
  }

  [[nodiscard]] const std::vector<StrategyOrder>& orders() const noexcept { return orders_; }

  [[nodiscard]] std::optional<StrategyOrder> find(OrderId id) const {
    for (const StrategyOrder& order : orders_) {
      if (order.id == id) {
        return order;
      }
    }
    return std::nullopt;
  }

  // Refreshes the behind estimate from the book's actual aggregate at each level.
  // Called after every venue event, because the denominator of the uniform cancel
  // assumption is only meaningful if it reflects the level as it stands.
  template <typename BookType>
  void refresh_behind(const BookType& book) {
    for (StrategyOrder& order : orders_) {
      const auto level_total =
          static_cast<double>(book.total_qty_at(order.side, order.price).raw());
      // The level total includes our own resting quantity, so remove it before
      // splitting the rest into ahead and behind.
      const double others = level_total - static_cast<double>(order.remaining);
      order.behind = others - order.ahead;
      if (order.behind < 0.0) {
        // The book says fewer shares rest here than we believe are ahead of us.
        // Trust the book: it is authoritative and the estimate is not.
        order.ahead = others < 0.0 ? 0.0 : others;
        order.behind = 0.0;
      }
    }
  }

  // Applies one venue event, returning any fills it produced.
  //
  // The fill model, stated explicitly because an over-optimistic one invalidates
  // every P&L number downstream:
  //
  //   1. **Trade at our price.** Shares consume the queue from the front. The
  //      quantity ahead of us absorbs first, and only the excess reaches us.
  //   2. **Trade through our price.** A resting order on our side at a price
  //      strictly worse than ours was executed. With our order genuinely present
  //      the aggressor would have taken us first, so we fill for the traded
  //      quantity, capped at our size.
  //   3. **Nothing else fills us.** In particular a passive venue add that
  //      crosses our quote does not, for the reasons in DESIGN.md, "The crossed
  //      book, and why the strategy tolerates one".
  std::vector<Fill> on_venue_event(const VenueEvent& event) {
    std::vector<Fill> fills;
    if (event.kind == VenueEvent::Kind::none || event.quantity == 0) {
      return fills;
    }

    for (StrategyOrder& order : orders_) {
      if (order.side != event.side || order.remaining == 0) {
        continue;
      }

      if (event.price == order.price) {
        if (event.kind == VenueEvent::Kind::execution) {
          apply_execution_at_price(order, event, fills);
        } else {
          apply_cancel_at_price(order, event);
        }
        continue;
      }

      if (event.kind == VenueEvent::Kind::execution && traded_through(order, event.price)) {
        const std::uint64_t quantity =
            event.quantity < order.remaining ? event.quantity : order.remaining;
        order.remaining -= quantity;
        fills.push_back(Fill{.id = order.id,
                             .side = order.side,
                             .price = order.price,
                             .quantity = quantity,
                             .timestamp_ns = event.timestamp_ns,
                             .from_trade_through = true});
      }
    }

    return fills;
  }

  // Drops fully filled orders. Kept separate from on_venue_event so a caller can
  // inspect the filled order before it disappears.
  void purge_filled() {
    std::size_t write = 0;
    for (const StrategyOrder& order : orders_) {
      if (order.remaining > 0) {
        orders_[write] = order;
        ++write;
      }
    }
    orders_.resize(write);
  }

 private:
  // True when `traded` is strictly worse for a resting order on this side than
  // the order's own price, which is the definition of the market having traded
  // through us. A resting bid is better the higher it is priced.
  [[nodiscard]] static bool traded_through(const StrategyOrder& order, Ticks traded) noexcept {
    return order.side == Side::buy ? traded < order.price : traded > order.price;
  }

  static void apply_execution_at_price(StrategyOrder& order,
                                       const VenueEvent& event,
                                       std::vector<Fill>& fills) {
    const auto traded = static_cast<double>(event.quantity);

    if (traded <= order.ahead) {
      order.ahead -= traded;
      return;
    }

    const double reaching_us = traded - order.ahead;
    order.ahead = 0.0;

    auto quantity = static_cast<std::uint64_t>(reaching_us);
    if (quantity > order.remaining) {
      quantity = order.remaining;
    }
    if (quantity == 0) {
      return;
    }

    order.remaining -= quantity;
    fills.push_back(Fill{.id = order.id,
                         .side = order.side,
                         .price = order.price,
                         .quantity = quantity,
                         .timestamp_ns = event.timestamp_ns,
                         .from_trade_through = false});
  }

  // The uniform cancel assumption, applied. See the class comment for why this
  // assumption and not one of the two extremes.
  static void apply_cancel_at_price(StrategyOrder& order, const VenueEvent& event) {
    const double total = order.ahead + order.behind;
    if (total <= 0.0) {
      return;
    }
    const auto cancelled = static_cast<double>(event.quantity);
    const double share_ahead = cancelled * (order.ahead / total);
    order.ahead -= share_ahead;
    if (order.ahead < 0.0) {
      order.ahead = 0.0;
    }
  }

  std::vector<StrategyOrder> orders_;
};

}  // namespace ob::strategy
