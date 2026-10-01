#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// Monotonic counter the engine stamps on each emitted event, so that an event
// stream has a total order independent of any clock.
//
// It does not establish queue priority. Time priority within a price level is
// structural: it is the insertion order of the intrusive list, so an Order carries
// no sequence field at all. Nothing compares Sequence values to decide who trades
// first.
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
// caller has already rejected prices that are not on a tick boundary and that are
// outside the representable domain (see representable()).
struct PriceConfig {
  std::int64_t tick_size = 100;      // scaled units per tick, ITCH penny = 100
  std::int64_t price_scale = 10000;  // scaled units per currency unit
  std::int64_t base_price = 0;       // scaled price mapped to tick index zero

  // Whether this configuration can be used at all. The conversions below have real
  // preconditions rather than merely conventional ones: tick_size is a divisor in
  // to_ticks and a modulus in on_tick_boundary, so a zero or negative tick_size is
  // undefined behaviour, not a wrong answer; price_scale divides scaled units into
  // currency and must be positive; and base_price must land on a tick boundary or
  // tick index zero would not map back to an exact price.
  //
  // Returns nullptr when the configuration is valid, or a static human readable
  // reason when it is not, so a caller can raise std::invalid_argument with a
  // specific message. Constructing a Book or Engine enforces this; the raw
  // conversion methods trust it, because they run on the API boundary of the hot
  // path and cannot afford to recheck.
  [[nodiscard]] constexpr const char* validity_error() const noexcept {
    if (tick_size <= 0) {
      return "PriceConfig.tick_size must be positive";
    }
    if (price_scale <= 0) {
      return "PriceConfig.price_scale must be positive";
    }
    if (base_price % tick_size != 0) {
      return "PriceConfig.base_price must be an exact multiple of tick_size";
    }
    return nullptr;
  }

  // valid() bounds the configuration's structure, not its arithmetic range. It
  // guarantees tick_size and price_scale are positive and base_price is on the
  // grid, which is what the conversions need to be meaningful. It does not promise
  // that every std::int64_t Price or every std::int32_t Ticks converts without
  // signal overflow: that is a per-value property the representable() helpers below
  // answer, and which the cold API boundary (Book construction, Engine command
  // validation) checks before calling the trusting conversions.
  [[nodiscard]] constexpr bool valid() const noexcept { return validity_error() == nullptr; }

  // Precondition: valid(), representable(price), and on_tick_boundary(price).
  // Undefined otherwise: price - base_price is a signed subtraction that overflows
  // for prices far from base_price, and the quotient is narrowed to 32 bits.
  [[nodiscard]] constexpr Ticks to_ticks(Price price) const noexcept {
    return Ticks{static_cast<std::int32_t>((price.raw() - base_price) / tick_size)};
  }

  // Precondition: valid() and tick_representable(ticks). Undefined otherwise:
  // ticks * tick_size and the addition of base_price are signed and can overflow.
  [[nodiscard]] constexpr Price to_price(Ticks ticks) const noexcept {
    return Price{base_price + (static_cast<std::int64_t>(ticks.raw()) * tick_size)};
  }

  // Precondition: valid() and representable(price). Undefined otherwise: it forms
  // the same price - base_price subtraction to_ticks does.
  [[nodiscard]] constexpr bool on_tick_boundary(Price price) const noexcept {
    return (price.raw() - base_price) % tick_size == 0;
  }

  // Whether to_ticks(price) and on_tick_boundary(price) can be evaluated without
  // signed overflow: price - base_price must fit in std::int64_t and the resulting
  // tick index must fit in std::int32_t. Computed without performing the
  // subtraction that could overflow, so it is safe to call on any Price.
  // Precondition: valid() (so tick_size > 0).
  //
  // This answers arithmetic representability only. Because the tick quotient
  // truncates toward zero, a price one to tick_size-1 raw units past an exact
  // endpoint is still representable() even though it is off-grid and past the last
  // whole tick. It is therefore not the command rejection classifier: callers use
  // the inclusive [min_representable_raw(), max_representable_raw()] interval, which
  // treats that endpoint fringe as out of range, so the engine and the reference
  // book classify it identically.
  [[nodiscard]] constexpr bool representable(Price price) const noexcept {
    const std::int64_t raw = price.raw();
    constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
    if (base_price > 0 && raw < I64_MIN + base_price) {
      return false;  // raw - base_price would underflow
    }
    if (base_price < 0 && raw > I64_MAX + base_price) {
      return false;  // raw - base_price would overflow
    }
    const std::int64_t tick = (raw - base_price) / tick_size;
    return tick >= std::numeric_limits<std::int32_t>::min() &&
           tick <= std::numeric_limits<std::int32_t>::max();
  }

  // Whether to_price(ticks) can be evaluated without signed overflow: ticks *
  // tick_size and its addition to base_price must both fit in std::int64_t. Safe to
  // call on any Ticks. Precondition: valid() (so tick_size > 0).
  [[nodiscard]] constexpr bool tick_representable(Ticks ticks) const noexcept {
    const std::int64_t t = ticks.raw();
    constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
    if (t > 0 && t > I64_MAX / tick_size) {
      return false;  // t * tick_size would overflow
    }
    if (t < 0 && t < I64_MIN / tick_size) {
      return false;  // t * tick_size would underflow
    }
    const std::int64_t product = t * tick_size;
    if (product > 0 && base_price > I64_MAX - product) {
      return false;  // base_price + product would overflow
    }
    if (product < 0 && base_price < I64_MIN - product) {
      return false;  // base_price + product would underflow
    }
    return true;
  }

  // The inclusive raw-price bounds for which to_ticks/on_tick_boundary are free of
  // signed overflow, so a hot-path caller can validate a price with two comparisons
  // instead of repeating the division representable() performs. They are the
  // narrower of the int32 tick range and the int64 subtraction range, and are
  // derived once at the cold boundary (Engine/Book construction) from base_price and
  // tick_size. For an on-tick price, min_representable_raw() <= raw <=
  // max_representable_raw() is exactly representable(price). Requires valid().
  [[nodiscard]] constexpr std::int64_t min_representable_raw() const noexcept {
    constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t I32_MIN = std::numeric_limits<std::int32_t>::min();
    const std::int64_t sub_floor = base_price > 0 ? I64_MIN + base_price : I64_MIN;
    if (I32_MIN < I64_MIN / tick_size) {
      return sub_floor;  // I32_MIN * tick_size underflows, so the subtraction binds
    }
    const std::int64_t product = I32_MIN * tick_size;
    if (base_price < I64_MIN - product) {
      return sub_floor;  // base_price + product underflows
    }
    const std::int64_t tick_floor = base_price + product;
    return tick_floor > sub_floor ? tick_floor : sub_floor;
  }

  [[nodiscard]] constexpr std::int64_t max_representable_raw() const noexcept {
    constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t I32_MAX = std::numeric_limits<std::int32_t>::max();
    const std::int64_t sub_ceil = base_price < 0 ? I64_MAX + base_price : I64_MAX;
    if (I32_MAX > I64_MAX / tick_size) {
      return sub_ceil;  // I32_MAX * tick_size overflows, so the subtraction binds
    }
    const std::int64_t product = I32_MAX * tick_size;
    if (base_price > I64_MAX - product) {
      return sub_ceil;  // base_price + product overflows
    }
    const std::int64_t tick_ceil = base_price + product;
    return tick_ceil < sub_ceil ? tick_ceil : sub_ceil;
  }
};

}  // namespace ob
