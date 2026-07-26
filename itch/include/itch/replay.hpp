#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "ob/book.hpp"
#include "ob/types.hpp"

namespace ob::itch {

struct ReplayStats {
  std::uint64_t messages_seen = 0;
  std::uint64_t messages_applied = 0;
  std::uint64_t other_symbol = 0;
  std::uint64_t unknown_type = 0;

  std::uint64_t adds = 0;
  std::uint64_t executions = 0;
  std::uint64_t cancels = 0;
  std::uint64_t deletes = 0;
  std::uint64_t replaces = 0;
  std::uint64_t trades = 0;
  std::uint64_t cross_trades = 0;
  std::uint64_t broken_trades = 0;
  std::uint64_t system_events = 0;

  // An add whose price fell outside the band and the cold cap, or which exhausted
  // the arena. Reported rather than silently dropped, because a non-zero value
  // means the book was sized wrong for the data and every downstream number is
  // suspect.
  std::uint64_t add_failures = 0;

  // A reduction or delete naming an order the book does not hold. Real streams
  // produce these legitimately at the start of a file, since the session began
  // before the capture did, so a small count is expected and a large one is not.
  std::uint64_t unknown_order_references = 0;

  [[nodiscard]] std::uint64_t book_mutations() const noexcept {
    return adds + executions + cancels + deletes + replaces;
  }
};

// Replays an ITCH 5.0 stream into a book, filtered to one symbol.
//
// What this does and deliberately does not do. It reconstructs the venue's book
// from the venue's own decisions; it does not re-run matching on historical
// orders. That distinction is the single most important thing about this file. An
// execution message is the venue telling us a trade already happened, so the
// correct response is to reduce the resting order by the executed size. Feeding
// historical orders through the matching engine instead would re-derive trades the
// stream has already reported and double count every one of them.
//
// Phase 5's strategy orders are the opposite case: those are hypothetical and have
// never been matched by anyone, so they do go through the engine and contend for
// real queue position against this reconstructed book.
//
// Symbol filtering is by stock locate code, not by comparing symbol strings. This
// is not an optimisation, it is a necessity: the execution, cancel, delete and
// replace messages carry no symbol field at all, only the locate. The locate to
// symbol mapping arrives in the stock directory messages at the start of the
// stream, so the driver watches for the directory entry naming the requested
// symbol and filters on the code it announces from then on.
template <std::size_t BandLevels>
class ReplayDriver {
 public:
  using BookType = ob::Book<BandLevels>;

  ReplayDriver(BookType& book, std::string symbol) : book_(&book), symbol_(std::move(symbol)) {}

  [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }

  [[nodiscard]] std::uint16_t resolved_locate() const noexcept { return locate_; }

  [[nodiscard]] bool symbol_resolved() const noexcept { return locate_resolved_; }

  // Nanoseconds since midnight from the most recently applied message. The engine
  // reads no clock, so this is what "now" means to anything downstream.
  [[nodiscard]] ob::Timestamp current_time() const noexcept { return current_time_; }

  void apply(const MessageView& message) noexcept {
    ++stats_.messages_seen;

    const char type = message.raw_type();

    // The directory has to be processed before filtering, since it is what makes
    // filtering possible in the first place.
    if (type == static_cast<char>(MessageType::stock_directory)) {
      handle_stock_directory(message);
      return;
    }

    if (type == static_cast<char>(MessageType::system_event)) {
      ++stats_.system_events;
      current_time_ = ob::Timestamp{message.timestamp()};
      return;
    }

    if (!is_known_type(type)) {
      ++stats_.unknown_type;
      return;
    }

    if (!locate_resolved_ || message.stock_locate() != locate_) {
      ++stats_.other_symbol;
      return;
    }

    current_time_ = ob::Timestamp{message.timestamp()};
    ++stats_.messages_applied;

    switch (static_cast<MessageType>(type)) {
      case MessageType::add_order:
      case MessageType::add_order_with_mpid:
        handle_add(message);
        return;

      // E and C both reduce the resting order by the executed size. They differ
      // only in that C carries the execution price and a printable flag, because
      // it reports a trade that printed at a price other than the order's own.
      // Neither difference changes what happens to the resting order.
      case MessageType::order_executed:
        handle_reduction(message, message.u32_at(order_executed::EXECUTED_SHARES));
        ++stats_.executions;
        return;
      case MessageType::order_executed_with_price:
        handle_reduction(message, message.u32_at(order_executed_with_price::EXECUTED_SHARES));
        ++stats_.executions;
        return;

      // X is a partial cancel: it reduces and only removes the order if the
      // reduction takes it to zero.
      case MessageType::order_cancel:
        handle_reduction(message, message.u32_at(order_cancel::CANCELLED_SHARES));
        ++stats_.cancels;
        return;

      case MessageType::order_delete:
        handle_delete(message);
        return;

      case MessageType::order_replace:
        handle_replace(message);
        return;

      // A non-cross trade message reports a trade against a non-displayed order.
      // There is no resting order in the visible book to adjust, so acting on it
      // would remove liquidity that was never there. It is counted and ignored.
      case MessageType::trade:
        ++stats_.trades;
        return;

      // Auction crosses execute outside the continuous book and broken trades are
      // after-the-fact corrections to a print. Neither touches resting liquidity.
      case MessageType::cross_trade:
        ++stats_.cross_trades;
        return;
      case MessageType::broken_trade:
        ++stats_.broken_trades;
        return;

      default:
        return;
    }
  }

 private:
  void handle_stock_directory(const MessageView& message) noexcept {
    const std::string_view symbol = message.symbol_at(stock_directory::STOCK);
    if (symbol != symbol_) {
      return;
    }
    locate_ = message.stock_locate();
    locate_resolved_ = true;
  }

  void handle_add(const MessageView& message) noexcept {
    ++stats_.adds;

    ob::AddRequest request;
    request.id = ob::OrderId{message.u64_at(add_order::ORDER_REFERENCE)};
    request.quantity = ob::Quantity{message.u32_at(add_order::SHARES)};
    request.timestamp = ob::Timestamp{message.timestamp()};
    request.side = message.char_at(add_order::BUY_SELL_INDICATOR) == BUY_INDICATOR ? ob::Side::buy
                                                                                   : ob::Side::sell;

    // ITCH prices are four byte integers with four implied decimals, which is the
    // same fixed point convention PriceConfig uses, so this is a widening cast and
    // not a conversion.
    const ob::Price price{static_cast<std::int64_t>(message.u32_at(add_order::PRICE))};
    if (!book_->price_config().on_tick_boundary(price)) {
      ++stats_.add_failures;
      return;
    }
    request.price = book_->price_config().to_ticks(price);

    if (book_->add(request) != ob::AddStatus::ok) {
      ++stats_.add_failures;
    }
  }

  // Shared by execution and partial cancel. The book stores an absolute remaining
  // quantity while ITCH reports a delta, so the current size has to be read before
  // it can be reduced.
  //
  // A reduction that meets or exceeds the remaining size removes the order. Using
  // the book's modify for that case instead would be wrong: modify rejects a
  // quantity of zero, and correctly so, since an order with no shares is not an
  // order.
  void handle_reduction(const MessageView& message, std::uint32_t shares) noexcept {
    const ob::OrderId id{message.u64_at(order_executed::ORDER_REFERENCE)};

    const ob::ArenaIndex index = book_->find_order(id);
    if (index == ob::INVALID_INDEX) {
      ++stats_.unknown_order_references;
      return;
    }

    const std::uint32_t remaining = book_->pool()[index].remaining;
    if (shares >= remaining) {
      static_cast<void>(book_->cancel(id));
      return;
    }

    static_cast<void>(book_->modify(id, ob::Quantity{remaining - shares}));
  }

  void handle_delete(const MessageView& message) noexcept {
    ++stats_.deletes;

    const ob::OrderId id{message.u64_at(order_delete::ORDER_REFERENCE)};
    if (book_->cancel(id) != ob::CancelStatus::ok) {
      ++stats_.unknown_order_references;
    }
  }

  // A replace is a delete of the original reference plus an add of a new one, and
  // the new order loses queue priority. It is not a modify, and the difference
  // matters: the message carries a new order reference number precisely because
  // the venue treats it as a new order. Implementing it as a modify would preserve
  // a queue position that the real book gave up, which would flatter every queue
  // position estimate Phase 5 produces.
  //
  // The side is not in the replace message. It comes from the order being
  // replaced, which is why the original has to be read before it is removed.
  void handle_replace(const MessageView& message) noexcept {
    ++stats_.replaces;

    const ob::OrderId original{message.u64_at(order_replace::ORIGINAL_ORDER_REFERENCE)};

    const ob::ArenaIndex index = book_->find_order(original);
    if (index == ob::INVALID_INDEX) {
      ++stats_.unknown_order_references;
      return;
    }

    const ob::Side side = book_->pool()[index].side;
    const ob::ParticipantId party = book_->pool()[index].party;

    static_cast<void>(book_->cancel(original));

    ob::AddRequest request;
    request.id = ob::OrderId{message.u64_at(order_replace::NEW_ORDER_REFERENCE)};
    request.quantity = ob::Quantity{message.u32_at(order_replace::SHARES)};
    request.timestamp = ob::Timestamp{message.timestamp()};
    request.side = side;
    request.party = party;

    const ob::Price price{static_cast<std::int64_t>(message.u32_at(order_replace::PRICE))};
    if (!book_->price_config().on_tick_boundary(price)) {
      ++stats_.add_failures;
      return;
    }
    request.price = book_->price_config().to_ticks(price);

    if (book_->add(request) != ob::AddStatus::ok) {
      ++stats_.add_failures;
    }
  }

  BookType* book_;
  std::string symbol_;
  ReplayStats stats_;
  ob::Timestamp current_time_{};
  std::uint16_t locate_ = 0;
  bool locate_resolved_ = false;
};

// Convenience wrapper: parse a whole buffer and replay it in one call.
template <std::size_t BandLevels>
ParseResult replay_buffer(std::span<const std::byte> data, ReplayDriver<BandLevels>& driver) {
  return for_each_message(data, [&driver](const MessageView& message) { driver.apply(message); });
}

}  // namespace ob::itch
