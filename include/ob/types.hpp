#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ob {

// Phantom tags. Never defined, only used to make otherwise identical StrongInt
// instantiations distinct types so the compiler rejects dimensional mistakes.
struct PriceTag;
struct TicksTag;
struct QuantityTag;
struct OrderIdTag;
struct TimestampTag;
struct SequenceTag;

// A zero overhead nominal type over an integer.
//
// Construction from the representation is explicit and extraction requires the
// named raw() call, so a Quantity can never silently become a Price. Only the
// operators that are dimensionally meaningful exist: same-type addition and
// subtraction, and scaling by a plain integer count. Cross-type arithmetic and
// cross-type conversion are both hard compile errors, which is the point.
template <typename Rep, typename Tag>
class StrongInt {
  static_assert(std::is_integral_v<Rep>, "StrongInt requires an integral representation");

 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongInt() noexcept = default;

  constexpr explicit StrongInt(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }

  friend constexpr auto operator<=>(const StrongInt&, const StrongInt&) noexcept = default;

  friend constexpr bool operator==(const StrongInt&, const StrongInt&) noexcept = default;

  constexpr StrongInt& operator+=(StrongInt other) noexcept {
    value_ = static_cast<Rep>(value_ + other.value_);
    return *this;
  }

  constexpr StrongInt& operator-=(StrongInt other) noexcept {
    value_ = static_cast<Rep>(value_ - other.value_);
    return *this;
  }

  [[nodiscard]] friend constexpr StrongInt operator+(StrongInt lhs, StrongInt rhs) noexcept {
    return StrongInt{static_cast<Rep>(lhs.value_ + rhs.value_)};
  }

  [[nodiscard]] friend constexpr StrongInt operator-(StrongInt lhs, StrongInt rhs) noexcept {
    return StrongInt{static_cast<Rep>(lhs.value_ - rhs.value_)};
  }

  // Scaling by a dimensionless count. Multiplying two quantities together
  // yields a quantity squared, which has no meaning here, so only the
  // integer-scalar form is provided.
  [[nodiscard]] friend constexpr StrongInt operator*(StrongInt lhs, Rep scalar) noexcept {
    return StrongInt{static_cast<Rep>(lhs.value_ * scalar)};
  }

 private:
  Rep value_{};
};

// Exchange price in scaled integer units, never floating point. The scale is
// carried by PriceConfig rather than baked in, because ITCH uses four implied
// decimals while other venues do not.
using Price = StrongInt<std::int64_t, PriceTag>;

// Signed tick index. This is the book's internal coordinate: the offset of a
// price from the band base measured in whole tick_size steps. Signed because
// rebasing and band-relative arithmetic both produce negatives transiently.
using Ticks = StrongInt<std::int32_t, TicksTag>;

// Share count. Sixty four bits so that per-level and per-book aggregates cannot
// overflow. Individual orders store a narrower field, see order.hpp.
using Quantity = StrongInt<std::uint64_t, QuantityTag>;

// Venue assigned order reference number. ITCH 5.0 uses eight bytes.
using OrderId = StrongInt<std::uint64_t, OrderIdTag>;

// Nanoseconds since midnight, matching the ITCH 5.0 timestamp semantic. The
// engine never reads a clock: every timestamp arrives on an input event.
using Timestamp = StrongInt<std::uint64_t, TimestampTag>;

// Monotonic arrival counter assigned by the engine. Time priority within a
// price level is resolved by this, not by Timestamp, because venue timestamps
// tie at nanosecond granularity under burst load.
using Sequence = StrongInt<std::uint64_t, SequenceTag>;

enum class Side : std::uint8_t { buy = 0, sell = 1 };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::buy ? Side::sell : Side::buy;
}

[[nodiscard]] constexpr std::size_t side_index(Side side) noexcept {
  return static_cast<std::size_t>(side);
}

// Sentinel arena index meaning "no order". Chosen as the maximum 32-bit value
// so that a valid index is always strictly less than the arena capacity, which
// makes the bounds check a single comparison.
using ArenaIndex = std::uint32_t;
constexpr ArenaIndex INVALID_INDEX = 0xFFFFFFFFU;

// Fixed point price conversion, applied at the API boundary only. Inside the
// book every price is a Ticks value, so no division appears on the hot path.
//
// Invariant: base_price is an exact multiple of tick_size. to_ticks assumes the
// caller has already rejected prices that are not on a tick boundary.
struct PriceConfig {
  std::int64_t tick_size = 100;      // scaled units per tick, ITCH penny = 100
  std::int64_t price_scale = 10000;  // scaled units per currency unit
  std::int64_t base_price = 0;       // scaled price mapped to tick index zero

  [[nodiscard]] constexpr Ticks to_ticks(Price price) const noexcept {
    return Ticks{static_cast<std::int32_t>((price.raw() - base_price) / tick_size)};
  }

  [[nodiscard]] constexpr Price to_price(Ticks ticks) const noexcept {
    return Price{base_price + (static_cast<std::int64_t>(ticks.raw()) * tick_size)};
  }

  [[nodiscard]] constexpr bool on_tick_boundary(Price price) const noexcept {
    return (price.raw() - base_price) % tick_size == 0;
  }
};

}  // namespace ob
