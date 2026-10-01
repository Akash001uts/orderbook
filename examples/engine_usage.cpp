// A minimal, runnable tour of the ob::Engine public API.
//
// It constructs an engine, submits a few commands, drains the events each one
// produces, and shows how an invalid configuration is reported. It is compiled as
// part of the normal build (target ob_example) so it cannot drift from the API, and
// it takes no arguments: run it and read the output.

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

namespace {

// Targeted using-declarations rather than a using-directive, so the example stays
// clang-tidy clean while reading like ordinary client code.
using ob::CancelNewest;
using ob::Command;
using ob::CommandType;
using ob::Engine;
using ob::EventRing;
using ob::EventType;
using ob::ExecutionEvent;
using ob::OrderId;
using ob::OrderType;
using ob::Price;
using ob::PriceConfig;
using ob::Quantity;
using ob::Side;
using ob::Ticks;
using ob::Timestamp;

// A book at the default band size holds a few megabytes of level array, so the
// library recommends constructing it on the heap. A 4096-level band is plenty for
// a demonstration and keeps the object small.
using DemoEngine = Engine<CancelNewest, 4096>;
using DemoRing = EventRing<64>;

const char* name_of(EventType type) {
  switch (type) {
    case EventType::fill:
      return "fill";
    case EventType::partial_fill:
      return "partial_fill";
    case EventType::cancelled:
      return "cancelled";
    case EventType::rejected:
      return "rejected";
    case EventType::book_update:
      return "book_update";
  }
  return "unknown";
}

// Drain every event the last command produced. The engine appended them to the
// ring; the caller decides what they mean.
void drain(DemoRing& ring) {
  ExecutionEvent event;
  while (ring.pop(event)) {
    std::cout << "    " << name_of(event.type) << " price=" << event.price.raw()
              << " qty=" << event.quantity.raw() << " taker=" << event.taker_id.raw()
              << " maker=" << event.maker_id.raw() << '\n';
  }
}

Command limit_add(std::uint64_t id, Side side, std::int64_t price, std::uint64_t quantity) {
  Command command;
  command.type = CommandType::add;
  command.order_type = OrderType::limit;
  command.id = OrderId{id};
  command.side = side;
  command.price = Price{price};
  command.quantity = Quantity{quantity};
  command.timestamp = Timestamp{id};
  return command;
}

Command market_add(std::uint64_t id, Side side, std::uint64_t quantity) {
  Command command = limit_add(id, side, 0, quantity);
  command.order_type = OrderType::market;
  return command;
}

}  // namespace

int main() {
  // 1. Construct an engine with a valid price configuration. Prices are scaled
  // integers; here a tick is 100 units and a currency unit is 10000 units.
  DemoEngine::Config config;
  config.price = PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 0};
  config.arena_capacity = 4096;
  config.max_cold_levels_per_side = 64;
  config.initial_center = Ticks{0};

  const auto engine = std::make_unique<DemoEngine>(config);

  // The sink is owned by the caller and must be large enough to hold every event a
  // single command can emit. Drain it between commands so each starts empty.
  DemoRing ring;

  // 2. Rest two sell orders. Each rests without trading, so each emits one
  // book_update.
  std::cout << "rest two asks:\n";
  ring.clear();
  engine->submit(limit_add(1, Side::sell, 10100, 50), ring);
  engine->submit(limit_add(2, Side::sell, 10200, 50), ring);
  drain(ring);

  // 3. A market buy for 80 shares consumes the 10100 ask in full and part of the
  // 10200 ask, producing a fill and a partial_fill.
  std::cout << "market buy 80:\n";
  ring.clear();
  engine->submit(market_add(3, Side::buy, 80), ring);
  drain(ring);

  // 4. An invalid configuration is reported by an exception at construction, before
  // any storage is allocated. submit itself never throws. If construction were to
  // succeed here the public contract would be broken, so the example exits nonzero
  // rather than reporting success: that makes it a genuine smoke test of the
  // rejection path rather than a program that always prints and returns zero.
  std::cout << "invalid configuration:\n";
  int status = 0;
  try {
    DemoEngine::Config bad = config;
    bad.price.tick_size = 0;  // a zero tick size cannot be used to convert prices
    DemoEngine broken(bad);
    std::cout << "    unexpected: construction did not throw\n";
    status = 1;
  } catch (const std::invalid_argument& error) {
    std::cout << "    rejected: " << error.what() << '\n';
  }

  return status;
}
