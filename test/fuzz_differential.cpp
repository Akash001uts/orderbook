#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "differential_harness.hpp"
#include "ob/events.hpp"

// Coverage guided differential fuzzing over the same command encoding the
// randomised test uses.
//
// Scope note, recorded so the brevity reads as a decision rather than an
// omission. The seeded differential test already explores this command space
// against the same oracle, and it explores it with a distribution chosen to keep
// the book tight and the matcher busy, which a coverage guided mutator has to
// discover for itself. What this target adds is a different search strategy:
// libFuzzer steers toward inputs that reach new branches, so it finds the narrow
// combinations a uniform sampler would need a very long run to stumble into.
// That is worth having, and it is worth about fifty lines rather than a
// subsystem, so this decodes bytes into commands and hands them to the shared
// harness. Everything else, including the comparison and the reproducer format,
// is the harness's.

namespace {

// Eight bytes per command. Fields are taken modulo their valid ranges rather than
// rejected when out of range: rejecting would make most mutations produce an
// empty input, which starves the fuzzer of feedback.
constexpr std::size_t BYTES_PER_COMMAND = 8;

// Deliberately narrow, for the same reason the randomised generator's span is
// narrow. A wide price range would spend the whole budget on orders that never
// meet each other.
constexpr std::int32_t PRICE_SPAN = 12;
constexpr std::uint64_t MAX_QUANTITY = 40;
constexpr std::uint64_t ID_SPACE = 64;
constexpr unsigned PARTICIPANTS = 3;

[[nodiscard]] ob::Command decode_command(const std::uint8_t* bytes, std::uint64_t sequence) {
  ob::Command command;

  switch (bytes[0] % 3U) {
    case 0:
      command.type = ob::CommandType::add;
      break;
    case 1:
      command.type = ob::CommandType::cancel;
      break;
    default:
      command.type = ob::CommandType::modify;
      break;
  }

  switch (bytes[1] % 5U) {
    case 0:
      command.order_type = ob::OrderType::limit;
      break;
    case 1:
      command.order_type = ob::OrderType::market;
      break;
    case 2:
      command.order_type = ob::OrderType::immediate_or_cancel;
      break;
    case 3:
      command.order_type = ob::OrderType::fill_or_kill;
      break;
    default:
      command.order_type = ob::OrderType::post_only;
      break;
  }

  command.side = (bytes[2] & 1U) == 0U ? ob::Side::buy : ob::Side::sell;

  // A small id space is what makes cancels and modifies hit live orders often
  // enough to matter. With a wide space almost every one would be an unknown
  // order rejection, which is a single branch already covered.
  command.id = ob::OrderId{1U + (static_cast<std::uint64_t>(bytes[3]) % ID_SPACE)};

  const auto tick =
      static_cast<std::int32_t>(static_cast<int>(bytes[4] % ((2U * PRICE_SPAN) + 1U)) - PRICE_SPAN);
  command.price = ob::Price{ob::testing::DIFF_BASE_PRICE +
                            (static_cast<std::int64_t>(tick) * ob::testing::DIFF_TICK_SIZE)};

  command.quantity = ob::Quantity{1U + (static_cast<std::uint64_t>(bytes[5]) % MAX_QUANTITY)};
  command.party = static_cast<ob::ParticipantId>(1U + (bytes[6] % PARTICIPANTS));

  // Timestamps come from the position in the stream, not from the input. The
  // engine never reads a clock, but a non-monotonic timestamp would still be a
  // property of the input rather than of the engine, and fuzzing it would explore
  // the generator instead of the book.
  command.timestamp = ob::Timestamp{sequence};

  return command;
}

}  // namespace

// The name is libFuzzer's entry point ABI, so it is not ours to style.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::size_t count = size / BYTES_PER_COMMAND;
  if (count == 0) {
    return 0;
  }

  std::vector<ob::Command> commands;
  commands.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    commands.push_back(decode_command(data + (i * BYTES_PER_COMMAND), i + 1U));
  }

  const ob::testing::ReplayResult result = ob::testing::replay(commands);
  if (result.mismatch.has_value()) {
    std::cerr << "differential mismatch at command " << result.failing_index << ": "
              << result.mismatch->description << '\n'
              << ob::testing::encode(commands);
    // Abort rather than return, so libFuzzer records the input as a crash and
    // writes it out. A silent return would let the fuzzer discard the one input
    // that mattered.
    std::abort();
  }

  return 0;
}
