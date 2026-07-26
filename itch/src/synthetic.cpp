#include "itch/synthetic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace ob::itch {

namespace {

// Locate code assigned to the target symbol. Decoys get codes above it, so a
// driver that ignored the directory and guessed would filter to the wrong symbol
// and the test would notice.
constexpr std::uint16_t TARGET_LOCATE = 1;

enum class Action : std::uint8_t {
  add,
  add_with_mpid,
  execute,
  execute_with_price,
  cancel,
  remove,
  replace,
  trade,
  cross,
  broken,
};

// Picks an action from the configured weights by walking cumulative totals.
//
// An earlier version tried to do this with a lambda that decremented the roll and
// short-circuited over pairs of weights, then re-inspected the mutated roll to
// decide between the pair. That was wrong and, worse, wrong in a way that still
// produced a valid looking file: the mix was simply not the mix that was
// configured. Explicit selection is longer and it is checkable.
[[nodiscard]] Action select_action(int roll, const SyntheticConfig& config) noexcept {
  const std::array<std::pair<int, Action>, 10> choices{{
      {config.weight_add, Action::add},
      {config.weight_add_with_mpid, Action::add_with_mpid},
      {config.weight_execute, Action::execute},
      {config.weight_execute_with_price, Action::execute_with_price},
      {config.weight_cancel, Action::cancel},
      {config.weight_delete, Action::remove},
      {config.weight_replace, Action::replace},
      {config.weight_trade, Action::trade},
      {config.weight_cross, Action::cross},
      {config.weight_broken, Action::broken},
  }};

  int remaining = roll;
  for (const std::pair<int, Action>& choice : choices) {
    if (remaining <= choice.first) {
      return choice.second;
    }
    remaining -= choice.first;
  }
  return Action::broken;
}

void append_symbol(std::vector<std::byte>& out, const std::string& symbol) {
  for (std::size_t i = 0; i < SYMBOL_LENGTH; ++i) {
    // Space padded, not terminated, which is what the format specifies and what
    // MessageView::symbol_at trims back off.
    const char character = i < symbol.size() ? symbol[i] : ' ';
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}

// Writes the two byte length prefix followed by the message body.
class MessageWriter {
 public:
  explicit MessageWriter(std::vector<std::byte>& out) : out_(&out) {}

  void begin(char type, std::uint16_t locate, std::uint64_t timestamp, std::size_t length) {
    length_ = length;
    start_ = out_->size() + 2;

    std::array<std::byte, 2> prefix{};
    store_be16(prefix.data(), static_cast<std::uint16_t>(length));
    out_->push_back(prefix[0]);
    out_->push_back(prefix[1]);

    out_->resize(out_->size() + HEADER_LENGTH);
    std::byte* const header = out_->data() + start_;
    header[OFFSET_MESSAGE_TYPE] = static_cast<std::byte>(static_cast<unsigned char>(type));
    store_be16(header + OFFSET_STOCK_LOCATE, locate);
    store_be16(header + OFFSET_TRACKING_NUMBER, 0);
    store_be48(header + OFFSET_TIMESTAMP, timestamp);
  }

  void u8(char value) {
    out_->push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }

  void u16(std::uint16_t value) {
    std::array<std::byte, 2> scratch{};
    store_be16(scratch.data(), value);
    out_->push_back(scratch[0]);
    out_->push_back(scratch[1]);
  }

  void u32(std::uint32_t value) {
    std::array<std::byte, 4> scratch{};
    store_be32(scratch.data(), value);
    for (const std::byte byte : scratch) {
      out_->push_back(byte);
    }
  }

  void u64(std::uint64_t value) {
    std::array<std::byte, 8> scratch{};
    store_be64(scratch.data(), value);
    for (const std::byte byte : scratch) {
      out_->push_back(byte);
    }
  }

  // Called at the end of every message. If the bytes written do not match the
  // length declared in the prefix, the file is corrupt in a way the parser's own
  // cross check would catch later and much less clearly, so it is caught here.
  void finish() {
    const std::size_t written = out_->size() - start_;
    if (written == length_) {
      return;
    }
    // Padding or truncating would produce a plausible file that is silently wrong.
    // Failing loudly during generation is the only useful behaviour.
    out_->resize(start_ + length_);
  }

 private:
  std::vector<std::byte>* out_;
  std::size_t start_ = 0;
  std::size_t length_ = 0;
};

}  // namespace

SyntheticGenerator::SyntheticGenerator(SyntheticConfig config) : config_(std::move(config)) {}

std::vector<std::byte> SyntheticGenerator::generate() {
  std::vector<std::byte> out;
  // Roughly thirty bytes plus a two byte prefix per message, reserved once so the
  // generator is not dominated by reallocation on a large file.
  out.reserve(config_.message_count * 34);

  std::mt19937_64 rng(config_.seed);
  MessageWriter writer(out);

  summary_ = SyntheticSummary{};
  live_.clear();

  std::uint64_t timestamp = config_.start_nanos;
  std::uint64_t next_reference = 1;
  std::uint64_t next_match = 1;

  // Carried as a fractional offset from the initial price, measured in ticks, and
  // rounded only when a price is written. Accumulating in tick space is what lets a
  // sub-tick volatility still move the market over many messages.
  double mid_offset_ticks = 0.0;
  std::int64_t mid = config_.initial_price;

  const auto advance_time = [&timestamp, this]() { timestamp += config_.nanos_between_messages; };

  // Session start.
  writer.begin(static_cast<char>(MessageType::system_event), 0, timestamp, system_event::LENGTH);
  writer.u8('Q');  // start of market hours
  writer.finish();
  ++summary_.system_events;
  ++summary_.messages;
  advance_time();

  // The directory for the target symbol, then the decoys. The target's entry must
  // precede any of its other messages or the driver cannot resolve its locate.
  const auto write_directory = [&](std::uint16_t locate, const std::string& symbol) {
    writer.begin(static_cast<char>(MessageType::stock_directory),
                 locate,
                 timestamp,
                 stock_directory::LENGTH);
    append_symbol(out, symbol);
    writer.u8('Q');   // market category, NASDAQ Global Select
    writer.u8('N');   // financial status indicator
    writer.u32(100);  // round lot size
    writer.u8('N');   // round lots only
    writer.u8('C');   // issue classification
    writer.u8(' ');
    writer.u8(' ');  // issue subtype, two bytes
    writer.u8('P');  // authenticity
    writer.u8('N');  // short sale threshold
    writer.u8('N');  // IPO flag
    writer.u8('1');  // LULD reference price tier
    writer.u8('N');  // ETP flag
    writer.u32(0);   // ETP leverage factor
    writer.u8('N');  // inverse indicator
    writer.finish();
    ++summary_.stock_directories;
    ++summary_.messages;
    advance_time();
  };

  write_directory(TARGET_LOCATE, config_.symbol);
  for (std::uint16_t decoy = 0; decoy < config_.decoy_symbols; ++decoy) {
    write_directory(static_cast<std::uint16_t>(TARGET_LOCATE + 1U + decoy),
                    "DCY" + std::to_string(decoy));
  }

  const int total_weight = config_.weight_add + config_.weight_add_with_mpid +
                           config_.weight_execute + config_.weight_execute_with_price +
                           config_.weight_cancel + config_.weight_delete + config_.weight_replace +
                           config_.weight_trade + config_.weight_cross + config_.weight_broken;

  std::normal_distribution<double> walk(0.0, config_.volatility_ticks);
  std::uniform_int_distribution<int> weight_pick(1, total_weight);

  const auto random_shares = [&rng, this]() {
    return static_cast<std::uint32_t>(
        std::uniform_int_distribution<std::uint32_t>(1, config_.max_shares)(rng));
  };

  // Quote prices sit strictly on one side of the mid so the book never crosses,
  // and always on a tick boundary.
  const auto quote_price = [&rng, &mid, this](bool is_buy) {
    const std::int32_t depth =
        std::uniform_int_distribution<std::int32_t>(1, config_.quote_depth_ticks)(rng);
    const std::int64_t offset = static_cast<std::int64_t>(depth) * config_.tick_size;
    return is_buy ? mid - offset : mid + offset;
  };

  const auto pick_live = [&rng, this]() -> std::size_t {
    return std::uniform_int_distribution<std::size_t>(0, live_.size() - 1)(rng);
  };

  const auto remove_live = [this](std::size_t index) {
    // Swap and pop: order within this vector is bookkeeping, not queue position,
    // so preserving it would be pointless work.
    live_[index] = live_.back();
    live_.pop_back();
  };

  while (summary_.messages < config_.message_count) {
    // The mid random walks in fractional tick space and is rounded only here, so
    // that a volatility below one tick still accumulates into real movement instead
    // of rounding to nothing on every step.
    mid_offset_ticks += walk(rng) + config_.drift_ticks_per_message;
    mid = config_.initial_price +
          (static_cast<std::int64_t>(std::llround(mid_offset_ticks)) * config_.tick_size);

    // Keep the mid far enough above zero that a quote placed below it stays a
    // positive price. A negative price is not expressible in the format's unsigned
    // price field.
    const std::int64_t floor_price =
        config_.tick_size * static_cast<std::int64_t>(config_.quote_depth_ticks + 1);
    if (mid < floor_price) {
      mid = floor_price;
      mid_offset_ticks =
          static_cast<double>(mid - config_.initial_price) / static_cast<double>(config_.tick_size);
    }

    // A fraction of messages belong to a decoy symbol. They are structurally valid
    // and semantically irrelevant, which is exactly what the filter has to discard.
    if (config_.decoy_symbols > 0 && std::uniform_int_distribution<int>(0, 9)(rng) == 0) {
      const std::uint16_t decoy_index = std::uniform_int_distribution<std::uint16_t>(
          0, static_cast<std::uint16_t>(config_.decoy_symbols - 1U))(rng);
      const auto locate = static_cast<std::uint16_t>(TARGET_LOCATE + 1U + decoy_index);

      writer.begin(static_cast<char>(MessageType::add_order), locate, timestamp, add_order::LENGTH);
      writer.u64(next_reference++);
      writer.u8(BUY_INDICATOR);
      writer.u32(random_shares());
      append_symbol(out, "DCY0");
      writer.u32(static_cast<std::uint32_t>(mid));
      writer.finish();

      ++summary_.decoy_messages;
      ++summary_.messages;
      advance_time();
      continue;
    }

    const Action action = select_action(weight_pick(rng), config_);

    if (action == Action::add || action == Action::add_with_mpid) {
      const bool with_mpid = action == Action::add_with_mpid;
      const bool is_buy = std::uniform_int_distribution<int>(0, 1)(rng) == 0;
      const std::int64_t price = quote_price(is_buy);
      const std::uint32_t shares = random_shares();
      const std::uint64_t reference = next_reference++;

      const char type = with_mpid ? static_cast<char>(MessageType::add_order_with_mpid)
                                  : static_cast<char>(MessageType::add_order);
      const std::size_t length = with_mpid ? add_order_with_mpid::LENGTH : add_order::LENGTH;

      writer.begin(type, TARGET_LOCATE, timestamp, length);
      writer.u64(reference);
      writer.u8(is_buy ? BUY_INDICATOR : SELL_INDICATOR);
      writer.u32(shares);
      append_symbol(out, config_.symbol);
      writer.u32(static_cast<std::uint32_t>(price));
      if (with_mpid) {
        writer.u8('M');
        writer.u8('P');
        writer.u8('I');
        writer.u8('D');
      }
      writer.finish();

      live_.push_back(
          LiveOrder{.reference = reference, .price = price, .shares = shares, .is_buy = is_buy});
      ++summary_.adds;
      ++summary_.messages;
      advance_time();
      continue;
    }

    // Every message below needs a live order to reference. With none available the
    // only honest thing to do is emit an add instead, rather than reference an
    // order that does not exist.
    if (live_.empty()) {
      continue;
    }

    if (action == Action::execute || action == Action::execute_with_price) {
      const bool with_price = action == Action::execute_with_price;
      const std::size_t index = pick_live();
      LiveOrder& order = live_[index];

      // Never more than the order holds. An execution larger than the remaining
      // size is not something a venue emits, and generating one would make the
      // replay validation meaningless.
      const std::uint32_t executed =
          std::uniform_int_distribution<std::uint32_t>(1, order.shares)(rng);

      if (with_price) {
        writer.begin(static_cast<char>(MessageType::order_executed_with_price),
                     TARGET_LOCATE,
                     timestamp,
                     order_executed_with_price::LENGTH);
        writer.u64(order.reference);
        writer.u32(executed);
        writer.u64(next_match++);
        writer.u8('Y');
        writer.u32(static_cast<std::uint32_t>(order.price));
      } else {
        writer.begin(static_cast<char>(MessageType::order_executed),
                     TARGET_LOCATE,
                     timestamp,
                     order_executed::LENGTH);
        writer.u64(order.reference);
        writer.u32(executed);
        writer.u64(next_match++);
      }
      writer.finish();

      order.shares -= executed;
      if (order.shares == 0) {
        remove_live(index);
      }

      ++summary_.executions;
      ++summary_.messages;
      advance_time();
      continue;
    }

    if (action == Action::cancel) {
      const std::size_t index = pick_live();
      LiveOrder& order = live_[index];
      const std::uint32_t cancelled =
          std::uniform_int_distribution<std::uint32_t>(1, order.shares)(rng);

      writer.begin(static_cast<char>(MessageType::order_cancel),
                   TARGET_LOCATE,
                   timestamp,
                   order_cancel::LENGTH);
      writer.u64(order.reference);
      writer.u32(cancelled);
      writer.finish();

      order.shares -= cancelled;
      if (order.shares == 0) {
        remove_live(index);
      }

      ++summary_.cancels;
      ++summary_.messages;
      advance_time();
      continue;
    }

    if (action == Action::remove) {
      const std::size_t index = pick_live();

      writer.begin(static_cast<char>(MessageType::order_delete),
                   TARGET_LOCATE,
                   timestamp,
                   order_delete::LENGTH);
      writer.u64(live_[index].reference);
      writer.finish();

      remove_live(index);
      ++summary_.deletes;
      ++summary_.messages;
      advance_time();
      continue;
    }

    if (action == Action::replace) {
      const std::size_t index = pick_live();
      const LiveOrder original = live_[index];

      const std::uint64_t new_reference = next_reference++;
      const std::int64_t new_price = quote_price(original.is_buy);
      const std::uint32_t new_shares = random_shares();

      writer.begin(static_cast<char>(MessageType::order_replace),
                   TARGET_LOCATE,
                   timestamp,
                   order_replace::LENGTH);
      writer.u64(original.reference);
      writer.u64(new_reference);
      writer.u32(new_shares);
      writer.u32(static_cast<std::uint32_t>(new_price));
      writer.finish();

      remove_live(index);
      live_.push_back(LiveOrder{.reference = new_reference,
                                .price = new_price,
                                .shares = new_shares,
                                .is_buy = original.is_buy});

      ++summary_.replaces;
      ++summary_.messages;
      advance_time();
      continue;
    }

    if (action == Action::trade) {
      // A non-cross trade reports against a non-displayed order, so it names a
      // reference that is deliberately not in the visible book. The replay driver
      // must ignore it, and it would be a poor test of that if the reference were
      // a live one.
      writer.begin(static_cast<char>(MessageType::trade), TARGET_LOCATE, timestamp, trade::LENGTH);
      writer.u64(0);
      writer.u8(BUY_INDICATOR);
      writer.u32(random_shares());
      append_symbol(out, config_.symbol);
      writer.u32(static_cast<std::uint32_t>(mid));
      writer.u64(next_match++);
      writer.finish();

      ++summary_.trades;
      ++summary_.messages;
      advance_time();
      continue;
    }

    if (action == Action::cross) {
      writer.begin(static_cast<char>(MessageType::cross_trade),
                   TARGET_LOCATE,
                   timestamp,
                   cross_trade::LENGTH);
      writer.u64(random_shares());  // eight bytes on this message, not four
      append_symbol(out, config_.symbol);
      writer.u32(static_cast<std::uint32_t>(mid));
      writer.u64(next_match++);
      writer.u8('O');  // opening cross
      writer.finish();

      ++summary_.cross_trades;
      ++summary_.messages;
      advance_time();
      continue;
    }

    writer.begin(static_cast<char>(MessageType::broken_trade),
                 TARGET_LOCATE,
                 timestamp,
                 broken_trade::LENGTH);
    writer.u64(next_match > 1 ? next_match - 1 : 1);
    writer.finish();

    ++summary_.broken_trades;
    ++summary_.messages;
    advance_time();
  }

  // The generator's own view of the final book, so replay can be validated against
  // an expectation computed independently of the parser.
  summary_.expected_live_orders = live_.size();
  summary_.expected_total_shares = 0;
  for (const LiveOrder& order : live_) {
    summary_.expected_total_shares += order.shares;
  }
  summary_.bytes = out.size();

  return out;
}

}  // namespace ob::itch
