// A minimal consumer of the orderbook library, used only to prove the public
// headers are reachable through the subproject include path and that the exported
// interface compiles. It is intentionally tiny: this check is about the build boundary,
// not the engine's behaviour, which the main test suite covers.
#include "ob/engine.hpp"
#include "ob/events.hpp"

int main() {
  ob::Engine<ob::CancelNewest>::Config config;
  config.arena_capacity = 16;
  ob::Engine<ob::CancelNewest> engine(config);

  ob::Command command;
  command.type = ob::CommandType::add;
  command.order_type = ob::OrderType::limit;
  command.side = ob::Side::buy;
  command.id = ob::OrderId{1};
  command.price = ob::Price{config.price.base_price};
  command.quantity = ob::Quantity{10};

  ob::EventRing<16> ring;
  engine.submit(command, ring);

  ob::ExecutionEvent event;
  int events = 0;
  while (ring.pop(event)) {
    ++events;
  }
  return events == 1 ? 0 : 1;
}
