#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "itch/messages.hpp"

namespace ob::itch {

// ---------------------------------------------------------------------------
// Big endian field decoding.
//
// Every load is assembled byte by byte. The tempting alternative, casting a
// pointer into the buffer to a struct or to a uint64 and byte swapping, is
// undefined behaviour twice over: it violates the strict aliasing rules and it
// performs an unaligned access. Unaligned loads happen to work on x86, which is
// exactly what makes the shortcut dangerous, because the code appears correct
// until it is compiled for a different target or run under ubsan.
//
// The byte assembly is not slow. Both GCC and Clang recognise this pattern and
// emit a single load plus a bswap instruction at -O2 and above.
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::uint64_t octet(const std::byte* data, std::size_t index) noexcept {
  return static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[index]));
}

[[nodiscard]] constexpr std::uint16_t load_be16(const std::byte* data) noexcept {
  return static_cast<std::uint16_t>((octet(data, 0) << 8U) | octet(data, 1));
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::byte* data) noexcept {
  return static_cast<std::uint32_t>((octet(data, 0) << 24U) | (octet(data, 1) << 16U) |
                                    (octet(data, 2) << 8U) | octet(data, 3));
}

// The six byte timestamp. There is no 48-bit integer type, so it is assembled
// into a uint64 and the top two bytes are simply never set.
[[nodiscard]] constexpr std::uint64_t load_be48(const std::byte* data) noexcept {
  return (octet(data, 0) << 40U) | (octet(data, 1) << 32U) | (octet(data, 2) << 24U) |
         (octet(data, 3) << 16U) | (octet(data, 4) << 8U) | octet(data, 5);
}

[[nodiscard]] constexpr std::uint64_t load_be64(const std::byte* data) noexcept {
  return (octet(data, 0) << 56U) | (octet(data, 1) << 48U) | (octet(data, 2) << 40U) |
         (octet(data, 3) << 32U) | (octet(data, 4) << 24U) | (octet(data, 5) << 16U) |
         (octet(data, 6) << 8U) | octet(data, 7);
}

void store_be16(std::byte* data, std::uint16_t value) noexcept;
void store_be32(std::byte* data, std::uint32_t value) noexcept;
void store_be48(std::byte* data, std::uint64_t value) noexcept;
void store_be64(std::byte* data, std::uint64_t value) noexcept;

// ---------------------------------------------------------------------------
// A view over one message.
//
// Zero copy: this holds a span into the mapped file and nothing else. Fields are
// decoded on demand by the accessors, so a callback that only looks at the type
// and the locate code pays for exactly those two loads and no more. That matters
// because replay filtered to a single symbol discards the large majority of
// messages after reading two fields.
// ---------------------------------------------------------------------------
class MessageView {
 public:
  constexpr MessageView() noexcept = default;

  constexpr explicit MessageView(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }

  [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept { return bytes_; }

  [[nodiscard]] constexpr char raw_type() const noexcept {
    return static_cast<char>(std::to_integer<std::uint8_t>(bytes_[OFFSET_MESSAGE_TYPE]));
  }

  [[nodiscard]] constexpr MessageType type() const noexcept {
    return static_cast<MessageType>(raw_type());
  }

  [[nodiscard]] constexpr std::uint16_t stock_locate() const noexcept {
    return load_be16(bytes_.data() + OFFSET_STOCK_LOCATE);
  }

  [[nodiscard]] constexpr std::uint16_t tracking_number() const noexcept {
    return load_be16(bytes_.data() + OFFSET_TRACKING_NUMBER);
  }

  // Nanoseconds since midnight, per the format.
  [[nodiscard]] constexpr std::uint64_t timestamp() const noexcept {
    return load_be48(bytes_.data() + OFFSET_TIMESTAMP);
  }

  // Generic accessors. The replay driver reads fields by offset constant from
  // messages.hpp rather than through a per-message wrapper type, because a
  // wrapper per message would be twenty near-identical classes whose only content
  // is the offsets that already exist.
  [[nodiscard]] constexpr std::uint16_t u16_at(std::size_t offset) const noexcept {
    return load_be16(bytes_.data() + offset);
  }

  [[nodiscard]] constexpr std::uint32_t u32_at(std::size_t offset) const noexcept {
    return load_be32(bytes_.data() + offset);
  }

  [[nodiscard]] constexpr std::uint64_t u64_at(std::size_t offset) const noexcept {
    return load_be64(bytes_.data() + offset);
  }

  [[nodiscard]] constexpr char char_at(std::size_t offset) const noexcept {
    return static_cast<char>(std::to_integer<std::uint8_t>(bytes_[offset]));
  }

  // Symbols are space padded to eight bytes rather than terminated, so the view is
  // trimmed here. It points into the mapped file and owns nothing.
  //
  // Not constexpr, and the reinterpret_cast is deliberate. std::byte and char are
  // both byte types, so viewing one as the other is explicitly permitted rather
  // than an aliasing violation, and it is the only way to hand out a
  // std::string_view without copying. The alternative, building a std::string,
  // would allocate once per symbol on a path that runs per message.
  [[nodiscard]] std::string_view symbol_at(std::size_t offset) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const char* const start = reinterpret_cast<const char*>(bytes_.data() + offset);
    std::size_t length = SYMBOL_LENGTH;
    while (length > 0 && start[length - 1] == ' ') {
      --length;
    }
    return {start, length};
  }

 private:
  std::span<const std::byte> bytes_;
};

// ---------------------------------------------------------------------------
// Framing.
// ---------------------------------------------------------------------------

enum class ParseStatus : std::uint8_t {
  ok,
  truncated_prefix,     // fewer than two bytes left, so no length can be read
  truncated_message,    // the prefix declares more bytes than remain
  length_disagreement,  // prefix and the known length for this type disagree
  implausible_length,   // zero, or longer than any message this format defines
};

struct ParseResult {
  ParseStatus status = ParseStatus::ok;
  std::size_t messages = 0;
  std::size_t bytes_consumed = 0;
  // Messages whose type this build does not recognise. They are skipped by their
  // declared length, which is the whole reason the prefix is trusted for unknown
  // types: without it an unrecognised message would desynchronise the stream.
  std::size_t unknown_types = 0;
};

// Iterates a length prefixed ITCH stream, calling the callback with a MessageView
// per message.
//
// The callback is a template parameter rather than a std::function so that it
// inlines into the loop. At tens of millions of messages per file the difference
// between an inlined predicate and an indirect call is the difference between
// measuring the parser and measuring the call.
//
// Taken by value rather than as a forwarding reference, because it is invoked once
// per message rather than once. A forwarding reference would invite a std::forward
// that is wrong here: forwarding an rvalue callable on the first of ten million
// calls would leave the remaining calls using a moved-from object.
//
// Framing: each message is preceded by a two byte big endian length covering the
// message body only, not the prefix itself. This is the framing NASDAQ's sample
// files use.
template <typename Callback>
[[nodiscard]] ParseResult for_each_message(std::span<const std::byte> data, Callback callback) {
  ParseResult result;
  std::size_t cursor = 0;

  while (cursor < data.size()) {
    if (data.size() - cursor < 2) {
      result.status = ParseStatus::truncated_prefix;
      break;
    }

    const std::size_t declared = load_be16(data.data() + cursor);
    if (declared == 0 || declared > MAX_MESSAGE_LENGTH) {
      result.status = ParseStatus::implausible_length;
      break;
    }

    cursor += 2;
    if (data.size() - cursor < declared) {
      result.status = ParseStatus::truncated_message;
      break;
    }

    const std::span<const std::byte> body = data.subspan(cursor, declared);
    const char type = static_cast<char>(std::to_integer<std::uint8_t>(body[OFFSET_MESSAGE_TYPE]));

    // Cross check the two independent sources of truth about length. A
    // disagreement means the stream is corrupt or this build's assumptions about
    // the protocol version are wrong, and either way decoding onward from a
    // plausible looking offset would produce confident nonsense.
    const std::size_t expected = message_length(type);
    if (expected != 0 && expected != declared) {
      result.status = ParseStatus::length_disagreement;
      break;
    }
    if (expected == 0) {
      ++result.unknown_types;
    }

    callback(MessageView(body));

    cursor += declared;
    ++result.messages;
    result.bytes_consumed = cursor;
  }

  return result;
}

}  // namespace ob::itch
