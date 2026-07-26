#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

namespace ob::testing {

// A deliberately naive limit order book, used only as a correctness oracle.
//
// Every choice here is the opposite of the one the real book makes. Levels are a
// std::map so the ordering is the container's problem rather than a bitmap
// invariant's. Each level is a std::list so queue order is the container's problem
// rather than an intrusive link's. Cancel finds its order by walking the list.
// Nothing is preallocated and everything allocates. It is O(n) where the real
// implementation is O(1) and that is entirely the point: this code is easy to read
// and hard to get wrong, so when the two disagree the fast one is wrong.
//
// What the differential comparison does and does not prove:
//
//   It proves the two implementations agree on matching semantics, on resulting
//   book state, and on what happened. Those are computed by structurally
//   unrelated code, so agreement is real evidence.
//
//   It does not prove the event protocol itself is right. Both sides emit the
//   same event shapes because the protocol is a design decision rather than a
//   derived fact, so a misconception about what an event should contain would be
//   shared. The unit tests in test_engine.cpp cover that separately, written from
//   the specification rather than from either implementation.
template <typename StpPolicy = CancelNewest>
class ReferenceBook {
 public:
  struct RestingOrder {
    OrderId id{};
    std::uint64_t remaining = 0;
    Timestamp timestamp{};
    ParticipantId party = NO_PARTICIPANT;
  };

  struct LevelView {
    Ticks price{};
    std::uint64_t aggregate_qty = 0;
    std::uint32_t order_count = 0;
  };

  explicit ReferenceBook(const PriceConfig& price_config) : price_config_(price_config) {}

  void submit(const Command& command, std::vector<ExecutionEvent>& out) {
    switch (command.type) {
      case CommandType::add:
        submit_add(command, out);
        return;
      case CommandType::cancel:
        submit_cancel(command, out);
        return;
      case CommandType::modify:
        submit_modify(command, out);
        return;
    }
  }

  // -------------------------------------------------------------------------
  // State inspection, for the comparison.
  // -------------------------------------------------------------------------

  [[nodiscard]] std::optional<Ticks> best_bid() const {
    const auto& levels = side_levels(Side::buy);
    if (levels.empty()) {
      return std::nullopt;
    }
    return levels.rbegin()->first;
  }

  [[nodiscard]] std::optional<Ticks> best_ask() const {
    const auto& levels = side_levels(Side::sell);
    if (levels.empty()) {
      return std::nullopt;
    }
    return levels.begin()->first;
  }

  [[nodiscard]] std::vector<LevelView> levels(Side side) const {
    std::vector<LevelView> view;
    for (const auto& entry : side_levels(side)) {
      LevelView level;
      level.price = entry.first;
      level.order_count = static_cast<std::uint32_t>(entry.second.size());
      for (const RestingOrder& order : entry.second) {
        level.aggregate_qty += order.remaining;
      }
      view.push_back(level);
    }
    return view;
  }

  [[nodiscard]] std::vector<std::uint64_t> ids_at(Side side, Ticks price) const {
    std::vector<std::uint64_t> ids;
    const auto& levels = side_levels(side);
    const auto found = levels.find(price);
    if (found == levels.end()) {
      return ids;
    }
    for (const RestingOrder& order : found->second) {
      ids.push_back(order.id.raw());
    }
    return ids;
  }

  [[nodiscard]] std::size_t live_count() const { return index_.size(); }

  [[nodiscard]] Sequence next_sequence() const { return next_sequence_; }

 private:
  struct Location {
    Side side = Side::buy;
    Ticks price{};
  };

  using LevelMap = std::map<Ticks, std::list<RestingOrder>>;

  [[nodiscard]] LevelMap& side_levels(Side side) { return sides_[side_index(side)]; }

  [[nodiscard]] const LevelMap& side_levels(Side side) const { return sides_[side_index(side)]; }

  // -------------------------------------------------------------------------
  // Events. Identical shapes to the engine, including the sequence counter, so
  // that the two streams can be compared element for element.
  // -------------------------------------------------------------------------

  void emit(std::vector<ExecutionEvent>& out, ExecutionEvent event) {
    event.sequence = next_sequence_;
    next_sequence_ += Sequence{1};
    out.push_back(event);
  }

  void emit_reject(std::vector<ExecutionEvent>& out, const Command& command, RejectReason reason) {
    ExecutionEvent event;
    event.type = EventType::rejected;
    event.reason = reason;
    event.taker_id = command.id;
    event.quantity = command.quantity;
    event.timestamp = command.timestamp;
    event.aggressor_side = command.side;
    emit(out, event);
  }

  void emit_cancel(std::vector<ExecutionEvent>& out,
                   OrderId id,
                   Ticks price,
                   std::uint64_t quantity,
                   Timestamp timestamp,
                   Side side,
                   RejectReason reason) {
    ExecutionEvent event;
    event.type = EventType::cancelled;
    event.reason = reason;
    event.taker_id = id;
    event.price = price;
    event.quantity = Quantity{quantity};
    event.timestamp = timestamp;
    event.aggressor_side = side;
    emit(out, event);
  }

  void emit_book_update(std::vector<ExecutionEvent>& out,
                        OrderId id,
                        Ticks price,
                        std::uint64_t quantity,
                        Timestamp timestamp,
                        Side side) {
    ExecutionEvent event;
    event.type = EventType::book_update;
    event.taker_id = id;
    event.price = price;
    event.quantity = Quantity{quantity};
    event.timestamp = timestamp;
    event.aggressor_side = side;
    emit(out, event);
  }

  // -------------------------------------------------------------------------
  // Add.
  // -------------------------------------------------------------------------

  void submit_add(const Command& command, std::vector<ExecutionEvent>& out) {
    if (command.id.raw() == 0U || index_.count(command.id.raw()) != 0U) {
      emit_reject(out, command, RejectReason::duplicate_order);
      return;
    }
    if (command.quantity.raw() == 0U) {
      emit_reject(out, command, RejectReason::zero_quantity);
      return;
    }
    if (command.quantity.raw() > MAX_ORDER_SHARES) {
      emit_reject(out, command, RejectReason::quantity_too_large);
      return;
    }

    const bool is_market = command.order_type == OrderType::market;

    Ticks limit{};
    if (!is_market) {
      if (!price_config_.on_tick_boundary(command.price)) {
        emit_reject(out, command, RejectReason::off_tick);
        return;
      }
      limit = price_config_.to_ticks(command.price);
    }

    if (command.order_type == OrderType::post_only && would_cross(command.side, limit)) {
      emit_reject(out, command, RejectReason::would_cross);
      return;
    }

    if (command.order_type == OrderType::fill_or_kill &&
        reachable_quantity(command.side, limit, is_market, command.quantity.raw(), command.party) <
            command.quantity.raw()) {
      emit_reject(out, command, RejectReason::insufficient_liquidity);
      return;
    }

    const Ticks event_price = is_market ? Ticks{0} : limit;

    std::uint64_t remaining = command.quantity.raw();
    const bool aggressor_cancelled = match(command, limit, is_market, remaining, out);

    if (remaining == 0U) {
      return;
    }

    if (aggressor_cancelled) {
      emit_cancel(out,
                  command.id,
                  event_price,
                  remaining,
                  command.timestamp,
                  command.side,
                  RejectReason::self_trade);
      return;
    }

    if (command.order_type != OrderType::limit && command.order_type != OrderType::post_only) {
      emit_cancel(out,
                  command.id,
                  event_price,
                  remaining,
                  command.timestamp,
                  command.side,
                  RejectReason::none);
      return;
    }

    RestingOrder order;
    order.id = command.id;
    order.remaining = remaining;
    order.timestamp = command.timestamp;
    order.party = command.party;

    side_levels(command.side)[limit].push_back(order);
    index_[command.id.raw()] = Location{command.side, limit};

    emit_book_update(out, command.id, limit, remaining, command.timestamp, command.side);
  }

  // Walks the opposing side from the best price outward. Because the container is
  // ordered, "outward" is just iteration: forward through the ask map for a buyer,
  // backward through the bid map for a seller.
  bool match(const Command& command,
             Ticks limit,
             bool is_market,
             std::uint64_t& remaining,
             std::vector<ExecutionEvent>& out) {
    const Side opposing = opposite(command.side);
    LevelMap& levels = side_levels(opposing);

    while (remaining > 0U && !levels.empty()) {
      const auto level_iterator =
          command.side == Side::buy ? levels.begin() : std::prev(levels.end());
      const Ticks price = level_iterator->first;

      if (!is_market && !crosses(command.side, limit, price)) {
        return false;
      }

      std::list<RestingOrder>& queue = level_iterator->second;
      auto order = queue.begin();

      while (order != queue.end() && remaining > 0U) {
        const StpAction action = StpPolicy::resolve(command.party, order->party);

        if (action == StpAction::cancel_aggressor) {
          return true;
        }

        if (action == StpAction::cancel_resting) {
          emit_cancel(out,
                      order->id,
                      price,
                      order->remaining,
                      order->timestamp,
                      opposing,
                      RejectReason::self_trade);
          index_.erase(order->id.raw());
          order = queue.erase(order);
          continue;
        }

        const std::uint64_t traded = order->remaining <= remaining ? order->remaining : remaining;
        const bool consumed = traded == order->remaining;

        ExecutionEvent fill;
        fill.taker_id = command.id;
        fill.maker_id = order->id;
        fill.quantity = Quantity{traded};
        fill.remaining = Quantity{order->remaining - traded};
        fill.timestamp = command.timestamp;
        fill.price = price;
        fill.aggressor_side = command.side;
        fill.type = consumed ? EventType::fill : EventType::partial_fill;

        remaining -= traded;

        if (consumed) {
          index_.erase(order->id.raw());
          order = queue.erase(order);
        } else {
          order->remaining -= traded;
          ++order;
        }

        emit(out, fill);
      }

      if (queue.empty()) {
        levels.erase(level_iterator);
      } else if (remaining > 0U) {
        // Liveness backstop, matching the engine's. Every path through the inner
        // loop fills, erases, or returns, so a level that still holds orders while
        // the aggressor has quantity left should be unreachable. Returning rather
        // than continuing means a broken invariant shows up as a differential
        // mismatch instead of hanging a ten million command test run.
        assert(false && "match made no progress at a level that still holds orders");
        return false;
      }
    }

    return false;
  }

  [[nodiscard]] std::uint64_t reachable_quantity(
      Side side, Ticks limit, bool is_market, std::uint64_t wanted, ParticipantId party) const {
    const Side opposing = opposite(side);
    const LevelMap& levels = side_levels(opposing);

    std::uint64_t total = 0;

    if (side == Side::buy) {
      for (auto level = levels.begin(); level != levels.end(); ++level) {
        if (!is_market && !crosses(side, limit, level->first)) {
          break;
        }
        if (accumulate_level(level->second, party, wanted, total)) {
          return total;
        }
      }
      return total;
    }

    for (auto level = levels.rbegin(); level != levels.rend(); ++level) {
      if (!is_market && !crosses(side, limit, level->first)) {
        break;
      }
      if (accumulate_level(level->second, party, wanted, total)) {
        return total;
      }
    }
    return total;
  }

  // Returns true when the walk must stop, either because the target is reached or
  // because self trade prevention would halt the aggressor here.
  [[nodiscard]] static bool accumulate_level(const std::list<RestingOrder>& queue,
                                             ParticipantId party,
                                             std::uint64_t wanted,
                                             std::uint64_t& total) {
    for (const RestingOrder& order : queue) {
      const StpAction action = StpPolicy::resolve(party, order.party);
      if (action == StpAction::cancel_aggressor) {
        return true;
      }
      if (action == StpAction::cancel_resting) {
        continue;
      }
      total += order.remaining;
      if (total >= wanted) {
        return true;
      }
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Cancel and modify.
  // -------------------------------------------------------------------------

  void submit_cancel(const Command& command, std::vector<ExecutionEvent>& out) {
    const auto located = index_.find(command.id.raw());
    if (located == index_.end()) {
      emit_reject(out, command, RejectReason::unknown_order);
      return;
    }

    const Location location = located->second;
    LevelMap& levels = side_levels(location.side);
    std::list<RestingOrder>& queue = levels.at(location.price);

    std::uint64_t quantity = 0;
    for (auto order = queue.begin(); order != queue.end(); ++order) {
      if (order->id == command.id) {
        quantity = order->remaining;
        queue.erase(order);
        break;
      }
    }

    if (queue.empty()) {
      levels.erase(location.price);
    }
    index_.erase(located);

    emit_cancel(out,
                command.id,
                location.price,
                quantity,
                command.timestamp,
                location.side,
                RejectReason::none);
  }

  void submit_modify(const Command& command, std::vector<ExecutionEvent>& out) {
    const auto located = index_.find(command.id.raw());
    if (located == index_.end()) {
      emit_reject(out, command, RejectReason::unknown_order);
      return;
    }
    if (command.quantity.raw() == 0U) {
      emit_reject(out, command, RejectReason::zero_quantity);
      return;
    }
    if (command.quantity.raw() > MAX_ORDER_SHARES) {
      emit_reject(out, command, RejectReason::quantity_too_large);
      return;
    }

    const Location location = located->second;
    std::list<RestingOrder>& queue = side_levels(location.side).at(location.price);

    const std::uint64_t wanted = command.quantity.raw();

    for (auto order = queue.begin(); order != queue.end(); ++order) {
      if (order->id != command.id) {
        continue;
      }

      if (wanted < order->remaining) {
        // A reduction keeps queue position.
        order->remaining = wanted;
      } else if (wanted > order->remaining) {
        // An increase goes to the back of the queue. Splicing the node to the end
        // is the list's way of saying the same thing the fast book says by
        // relinking within its arena slot.
        RestingOrder moved = *order;
        moved.remaining = wanted;
        queue.erase(order);
        queue.push_back(moved);
      }
      break;
    }

    emit_book_update(out, command.id, location.price, wanted, command.timestamp, location.side);
  }

  // -------------------------------------------------------------------------
  // Crossing predicates, stated exactly as the specification states them.
  // -------------------------------------------------------------------------

  [[nodiscard]] static bool crosses(Side aggressor, Ticks limit, Ticks resting) {
    return aggressor == Side::buy ? resting <= limit : resting >= limit;
  }

  [[nodiscard]] bool would_cross(Side side, Ticks limit) const {
    const std::optional<Ticks> best = side == Side::buy ? best_ask() : best_bid();
    return best.has_value() && crosses(side, limit, *best);
  }

  PriceConfig price_config_;
  std::array<LevelMap, 2> sides_{};
  std::unordered_map<std::uint64_t, Location> index_;
  Sequence next_sequence_{1};
};

}  // namespace ob::testing
