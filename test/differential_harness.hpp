#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "reference_book.hpp"

// Shared machinery for the differential comparison, used by both the GoogleTest
// harness and the libFuzzer target so that the two cannot drift apart. If the
// fuzzer compared state differently from the test, a bug it found might not
// reproduce under the test, which would make the fuzzer's findings unusable.
namespace ob::testing {

using DiffEngine = Engine<CancelNewest, 4096>;
using DiffReference = ReferenceBook<CancelNewest>;

constexpr std::int64_t DIFF_BASE_PRICE = 1000000;
constexpr std::int64_t DIFF_TICK_SIZE = 100;
constexpr std::size_t DIFF_RING_CAPACITY = 4096;

using DiffRing = EventRing<DIFF_RING_CAPACITY>;

[[nodiscard]] inline PriceConfig diff_price_config() {
  return PriceConfig{
      .tick_size = DIFF_TICK_SIZE, .price_scale = 10000, .base_price = DIFF_BASE_PRICE};
}

[[nodiscard]] inline DiffEngine::Config diff_engine_config() {
  DiffEngine::Config config;
  config.price = diff_price_config();
  config.arena_capacity = 8192;
  config.max_cold_levels_per_side = 64;
  config.initial_center = Ticks{0};
  return config;
}

struct Mismatch {
  std::string description;
};

struct ReplayResult {
  std::optional<Mismatch> mismatch;
  std::size_t failing_index = 0;
};

[[nodiscard]] inline std::string describe_price(const std::optional<Ticks>& price) {
  return price.has_value() ? std::to_string(price->raw()) : std::string("none");
}

[[nodiscard]] inline std::optional<Mismatch> compare_events(
    const std::vector<ExecutionEvent>& fast, const std::vector<ExecutionEvent>& reference) {
  if (fast.size() != reference.size()) {
    std::ostringstream out;
    out << "event count differs: engine " << fast.size() << ", reference " << reference.size();
    return Mismatch{out.str()};
  }

  for (std::size_t i = 0; i < fast.size(); ++i) {
    const ExecutionEvent& lhs = fast[i];
    const ExecutionEvent& rhs = reference[i];

    if (lhs.type == rhs.type && lhs.reason == rhs.reason && lhs.taker_id == rhs.taker_id &&
        lhs.maker_id == rhs.maker_id && lhs.quantity == rhs.quantity &&
        lhs.remaining == rhs.remaining && lhs.price == rhs.price && lhs.sequence == rhs.sequence &&
        lhs.aggressor_side == rhs.aggressor_side) {
      continue;
    }

    std::ostringstream out;
    out << "event " << i << " differs: engine {type=" << static_cast<int>(lhs.type)
        << " reason=" << static_cast<int>(lhs.reason) << " taker=" << lhs.taker_id.raw()
        << " maker=" << lhs.maker_id.raw() << " qty=" << lhs.quantity.raw()
        << " rem=" << lhs.remaining.raw() << " px=" << lhs.price.raw()
        << " seq=" << lhs.sequence.raw() << "}, reference {type=" << static_cast<int>(rhs.type)
        << " reason=" << static_cast<int>(rhs.reason) << " taker=" << rhs.taker_id.raw()
        << " maker=" << rhs.maker_id.raw() << " qty=" << rhs.quantity.raw()
        << " rem=" << rhs.remaining.raw() << " px=" << rhs.price.raw()
        << " seq=" << rhs.sequence.raw() << "}";
    return Mismatch{out.str()};
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional<Mismatch> compare_side(const DiffEngine& engine,
                                                          const DiffReference& reference,
                                                          Side side) {
  std::vector<DiffEngine::BookType::LevelSnapshot> fast;
  engine.book().snapshot_levels(side, fast);
  const std::vector<DiffReference::LevelView> slow = reference.levels(side);

  if (fast.size() != slow.size()) {
    std::ostringstream out;
    out << "occupied level count differs on side " << static_cast<int>(side) << ": engine "
        << fast.size() << ", reference " << slow.size();
    return Mismatch{out.str()};
  }

  for (std::size_t i = 0; i < fast.size(); ++i) {
    if (fast[i].price != slow[i].price) {
      std::ostringstream out;
      out << "level " << i << " price differs on side " << static_cast<int>(side) << ": engine "
          << fast[i].price.raw() << ", reference " << slow[i].price.raw();
      return Mismatch{out.str()};
    }

    if (fast[i].aggregate_qty.raw() != slow[i].aggregate_qty) {
      std::ostringstream out;
      out << "aggregate quantity differs at price " << fast[i].price.raw() << ": engine "
          << fast[i].aggregate_qty.raw() << ", reference " << slow[i].aggregate_qty;
      return Mismatch{out.str()};
    }

    if (fast[i].order_count != slow[i].order_count) {
      std::ostringstream out;
      out << "order count differs at price " << fast[i].price.raw() << ": engine "
          << fast[i].order_count << ", reference " << slow[i].order_count;
      return Mismatch{out.str()};
    }

    // The strongest of these checks. Aggregates can agree while queue order is
    // wrong, and queue order is what price-time priority actually promises, so
    // the exact sequence of ids at every occupied level is compared.
    std::vector<std::uint64_t> fast_ids;
    for (ArenaIndex index = engine.book().first_order_at(side, fast[i].price);
         index != INVALID_INDEX;
         index = engine.book().pool()[index].next) {
      fast_ids.push_back(engine.book().pool()[index].id.raw());
    }
    const std::vector<std::uint64_t> slow_ids = reference.ids_at(side, fast[i].price);

    if (fast_ids != slow_ids) {
      std::ostringstream out;
      out << "queue order differs at price " << fast[i].price.raw() << ": engine [";
      for (const std::uint64_t id : fast_ids) {
        out << id << ' ';
      }
      out << "], reference [";
      for (const std::uint64_t id : slow_ids) {
        out << id << ' ';
      }
      out << ']';
      return Mismatch{out.str()};
    }
  }

  return std::nullopt;
}

[[nodiscard]] inline std::optional<Mismatch> compare_state(const DiffEngine& engine,
                                                           const DiffReference& reference) {
  if (engine.book().best_bid() != reference.best_bid()) {
    return Mismatch{"best bid differs: engine " + describe_price(engine.book().best_bid()) +
                    ", reference " + describe_price(reference.best_bid())};
  }
  if (engine.book().best_ask() != reference.best_ask()) {
    return Mismatch{"best ask differs: engine " + describe_price(engine.book().best_ask()) +
                    ", reference " + describe_price(reference.best_ask())};
  }
  if (engine.book().pool().live_count() != reference.live_count()) {
    std::ostringstream out;
    out << "live order count differs: engine " << engine.book().pool().live_count()
        << ", reference " << reference.live_count();
    return Mismatch{out.str()};
  }

  if (const std::optional<Mismatch> buy = compare_side(engine, reference, Side::buy)) {
    return buy;
  }
  return compare_side(engine, reference, Side::sell);
}

// Feeds one command to both implementations and compares everything. Returns a
// description of the first disagreement, or nothing.
[[nodiscard]] inline std::optional<Mismatch> step(const Command& command,
                                                  DiffEngine& engine,
                                                  DiffReference& reference,
                                                  DiffRing& ring,
                                                  std::vector<ExecutionEvent>& fast_events,
                                                  std::vector<ExecutionEvent>& slow_events) {
  ring.clear();
  fast_events.clear();
  slow_events.clear();

  engine.submit(command, ring);
  reference.submit(command, slow_events);

  ExecutionEvent event;
  while (ring.pop(event)) {
    fast_events.push_back(event);
  }

  if (ring.overflowed()) {
    return Mismatch{
        "event ring overflowed, so events were dropped and the comparison would be "
        "meaningless"};
  }

  if (std::optional<Mismatch> events = compare_events(fast_events, slow_events)) {
    return events;
  }
  return compare_state(engine, reference);
}

[[nodiscard]] inline ReplayResult replay(const std::vector<Command>& commands) {
  DiffEngine engine(diff_engine_config());
  DiffReference reference(diff_price_config());
  DiffRing ring;

  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t i = 0; i < commands.size(); ++i) {
    if (std::optional<Mismatch> mismatch =
            step(commands[i], engine, reference, ring, fast_events, slow_events)) {
      return ReplayResult{mismatch, i};
    }
  }

  return ReplayResult{std::nullopt, 0};
}

// -----------------------------------------------------------------------------
// The reproducer format. Plain text on purpose: a committed regression should be
// readable in a diff and editable by hand when narrowing a bug further.
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::string encode(const std::vector<Command>& commands) {
  std::ostringstream out;
  out << "# Minimal reproducer for a differential mismatch.\n"
      << "# Fields: type order_type side id price quantity timestamp party\n"
      << "# type: 0 add, 1 cancel, 2 modify\n"
      << "# order_type: 0 limit, 1 market, 2 ioc, 3 fok, 4 post_only\n"
      << "# side: 0 buy, 1 sell\n";
  for (const Command& command : commands) {
    out << static_cast<int>(command.type) << ' ' << static_cast<int>(command.order_type) << ' '
        << static_cast<int>(command.side) << ' ' << command.id.raw() << ' ' << command.price.raw()
        << ' ' << command.quantity.raw() << ' ' << command.timestamp.raw() << ' ' << command.party
        << '\n';
  }
  return out.str();
}

[[nodiscard]] inline std::vector<Command> decode(std::istream& input) {
  std::vector<Command> commands;
  std::string line;

  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream fields(line);
    int type = 0;
    int order_type = 0;
    int side = 0;
    std::uint64_t id = 0;
    std::int64_t price = 0;
    std::uint64_t quantity = 0;
    std::uint64_t timestamp = 0;
    unsigned party = 0;

    if (!(fields >> type >> order_type >> side >> id >> price >> quantity >> timestamp >> party)) {
      continue;
    }

    Command command;
    command.type = static_cast<CommandType>(type);
    command.order_type = static_cast<OrderType>(order_type);
    command.side = static_cast<Side>(side);
    command.id = OrderId{id};
    command.price = Price{price};
    command.quantity = Quantity{quantity};
    command.timestamp = Timestamp{timestamp};
    command.party = static_cast<ParticipantId>(party);
    commands.push_back(command);
  }

  return commands;
}

}  // namespace ob::testing
