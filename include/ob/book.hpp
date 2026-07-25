#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ob/level_bitmap.hpp"
#include "ob/order.hpp"
#include "ob/order_map.hpp"
#include "ob/order_pool.hpp"
#include "ob/price_level.hpp"
#include "ob/types.hpp"

namespace ob {

// Overflow storage for price levels outside the band.
//
// This is the cold path and it is the only part of the book backed by a node based
// ordered container. It exists so that a price arriving a million ticks away is a
// slow correct operation rather than a rejection or a crash. Everything about it
// is the opposite of the hot path: it allocates, it chases pointers, and it is
// O(log n).
//
// The implementation is hidden behind a pointer so that <map> never enters this
// header. Nothing on the hot path should be able to reach a std::map by accident,
// and the cleanest way to guarantee that is for the declaration not to be visible.
class ColdLevels {
 public:
  explicit ColdLevels(std::size_t max_levels_per_side);
  ~ColdLevels();

  ColdLevels(const ColdLevels&) = delete;
  ColdLevels& operator=(const ColdLevels&) = delete;
  ColdLevels(ColdLevels&&) = delete;
  ColdLevels& operator=(ColdLevels&&) = delete;

  // Returns nullptr when the per side level cap is already reached, which the
  // caller reports as band_overflow. The cap is what keeps cold memory bounded
  // and known at construction rather than unbounded at runtime.
  [[nodiscard]] PriceLevel* level_for(Side side, Ticks price);

  [[nodiscard]] PriceLevel* find(Side side, Ticks price) noexcept;
  [[nodiscard]] const PriceLevel* find(Side side, Ticks price) const noexcept;

  void erase(Side side, Ticks price) noexcept;

  [[nodiscard]] bool empty(Side side) const noexcept;
  [[nodiscard]] std::size_t size(Side side) const noexcept;

  // Number of levels within the inclusive range. Rebasing needs this to know how
  // much cold capacity extracting that range will free before it starts evicting
  // into cold.
  [[nodiscard]] std::size_t count_in_range(Side side, Ticks low, Ticks high) const noexcept;

  // Highest price for the buy side, lowest for the sell side, matching the sense
  // of "best" on each. False when that side holds nothing.
  [[nodiscard]] bool best(Side side, Ticks& out) const noexcept;

  // Removes every level whose price falls within the inclusive range and appends
  // them to out. Used only by band rebasing, to reclaim levels that the new band
  // now covers.
  void extract_range(Side side,
                     Ticks low,
                     Ticks high,
                     std::vector<std::pair<Ticks, PriceLevel>>& out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class AddStatus : std::uint8_t {
  ok,
  invalid_id,
  duplicate_id,
  zero_quantity,
  quantity_too_large,
  arena_exhausted,
  map_exhausted,
  band_overflow,
};

enum class CancelStatus : std::uint8_t { ok, unknown_order };

enum class ModifyStatus : std::uint8_t {
  reduced_in_place,  // quantity fell, queue position preserved
  requeued,          // quantity rose, order moved to the back of its level
  unchanged,
  unknown_order,
  zero_quantity,
  quantity_too_large,
};

struct AddRequest {
  OrderId id{};
  Ticks price{};
  Quantity quantity{};
  Timestamp timestamp{};
  ParticipantId party = NO_PARTICIPANT;
  Side side = Side::buy;
};

constexpr std::size_t DEFAULT_BAND_LEVELS = 65536;

// A single symbol limit order book.
//
// The structure is a flat array of price levels per side, indexed directly by the
// offset of a price from a band base, rather than a tree keyed by price. The
// reasoning, the memory cost accepted, and the workload that would make this the
// wrong choice are all in DESIGN.md.
//
// Sizing note: two level arrays plus the two occupancy bitmaps put roughly 3 MiB
// of state in this object at the default band size, and the order arena adds more
// behind a pointer. Construct it on the heap.
template <std::size_t BandLevels = DEFAULT_BAND_LEVELS>
class Book {
 public:
  using Bitmap = LevelBitmap<BandLevels>;

  static constexpr std::size_t BAND_LEVELS = BandLevels;

  // How close the market may come to a band edge before the band is recentred.
  // An eighth of the band is roughly 8192 ticks at the default size, which is far
  // more than a normal session's drift in a single symbol, so rebasing stays the
  // rare event it is designed to be.
  static constexpr std::size_t REBASE_MARGIN = BandLevels / 8;

  // Arena capacity is a cache residency decision as much as a capacity one, and
  // that is easy to miss. Every slot is 40 bytes and the id map adds another 24
  // per slot at the design load factor, so 2^16 orders costs roughly 4 MiB across
  // the two and 2^20 costs roughly 64 MiB. Measured on an 8 MiB L3, an add costs
  // about 18 ns while that working set fits in L2 and about 130 ns when it does
  // not. The default is therefore sized to a busy single symbol rather than to the
  // largest book imaginable; oversizing it is not free headroom, it is a seven
  // times slowdown on every operation.
  struct Config {
    PriceConfig price{};
    std::uint32_t arena_capacity = 1U << 16;
    std::size_t max_cold_levels_per_side = 4096;
    Ticks initial_center{0};
  };

  explicit Book(const Config& config)
      : pool_(config.arena_capacity),
        id_map_(config.arena_capacity),
        cold_(config.max_cold_levels_per_side),
        price_config_(config.price),
        band_base_(centre_to_base(config.initial_center)),
        max_cold_levels_(config.max_cold_levels_per_side) {
    for (std::size_t side = 0; side < 2; ++side) {
      levels_[side].assign(BandLevels, PriceLevel{});
    }
    rebase_scratch_.assign(BandLevels, 0U);
    cold_scratch_.reserve(config.max_cold_levels_per_side);
  }

  // -------------------------------------------------------------------------
  // Hot path. No allocation, no exceptions, no clock, no syscalls, no logging.
  //
  // The one qualification: an add whose price lands outside the band reaches
  // ColdLevels, which allocates. That branch is documented as the cold path and
  // the tests measure it separately from the in-band path precisely so the
  // distinction stays honest rather than assumed.
  // -------------------------------------------------------------------------

  [[nodiscard]] AddStatus add(const AddRequest& request) noexcept {
    if (request.id.raw() == 0U) {
      return AddStatus::invalid_id;
    }
    if (request.quantity.raw() == 0U) {
      return AddStatus::zero_quantity;
    }
    if (request.quantity.raw() > MAX_ORDER_SHARES) {
      return AddStatus::quantity_too_large;
    }
    if (id_map_.find(request.id) != INVALID_INDEX) {
      return AddStatus::duplicate_id;
    }

    const ArenaIndex index = pool_.allocate();
    if (index == INVALID_INDEX) {
      return AddStatus::arena_exhausted;
    }

    Order& order = pool_[index];
    order.id = request.id;
    order.timestamp = request.timestamp;
    order.price = request.price;
    order.remaining = static_cast<std::uint32_t>(request.quantity.raw());
    order.party = request.party;
    order.side = request.side;

    if (!id_map_.insert(request.id, index)) {
      pool_.deallocate(index);
      return AddStatus::map_exhausted;
    }

    if (in_band(request.price)) {
      const std::size_t slot = slot_of(request.price);
      PriceLevel& level = levels_[side_index(request.side)][slot];
      const bool was_empty = level.empty();
      link_at_tail(level, index);
      if (was_empty) {
        occupied_[side_index(request.side)].set(slot);
      }
      maybe_rebase(slot);
      return AddStatus::ok;
    }

    PriceLevel* level = cold_.level_for(request.side, request.price);
    if (level == nullptr) {
      const bool erased = id_map_.erase(request.id);
      static_cast<void>(erased);
      assert(erased);
      pool_.deallocate(index);
      return AddStatus::band_overflow;
    }

    link_at_tail(*level, index);
    ++cold_operations_;
    refresh_cold_counts();
    cold_pending_ = true;
    rebase_now();
    return AddStatus::ok;
  }

  [[nodiscard]] CancelStatus cancel(OrderId id) noexcept {
    const ArenaIndex index = id_map_.find(id);
    if (index == INVALID_INDEX) {
      return CancelStatus::unknown_order;
    }

    remove_from_level(index);

    const bool erased = id_map_.erase(id);
    static_cast<void>(erased);
    assert(erased);

    pool_.deallocate(index);
    return CancelStatus::ok;
  }

  // Exchange-correct modify semantics, and the reason they are what they are:
  //
  // A quantity decrease is a reduction of an existing commitment, so the order
  // keeps its place in the queue. Mutating remaining in place and adjusting the
  // level aggregate is all that is required.
  //
  // A quantity increase is a new commitment for the added shares. Real venues do
  // not let a participant buy queue priority by resting one share and inflating it
  // later, so the order goes to the back of its level. This is implemented by
  // relinking within the same arena slot rather than by a cancel plus an add:
  // priority is lost either way, and reusing the slot avoids touching the id map
  // and the free list for no benefit.
  //
  // A price change is not expressible here. It is a cancel plus an add by
  // definition, since the order belongs to a different level afterwards, and the
  // caller performs both so that the event stream records both.
  [[nodiscard]] ModifyStatus modify(OrderId id, Quantity new_quantity) noexcept {
    if (new_quantity.raw() == 0U) {
      return ModifyStatus::zero_quantity;
    }
    if (new_quantity.raw() > MAX_ORDER_SHARES) {
      return ModifyStatus::quantity_too_large;
    }

    const ArenaIndex index = id_map_.find(id);
    if (index == INVALID_INDEX) {
      return ModifyStatus::unknown_order;
    }

    Order& order = pool_[index];
    const auto wanted = static_cast<std::uint32_t>(new_quantity.raw());
    if (wanted == order.remaining) {
      return ModifyStatus::unchanged;
    }

    PriceLevel* const level = level_for_order(order);
    assert(level != nullptr);

    if (wanted < order.remaining) {
      level->aggregate_qty -= Quantity{order.remaining - wanted};
      order.remaining = wanted;
      return ModifyStatus::reduced_in_place;
    }

    unlink(*level, index);
    order.remaining = wanted;
    link_at_tail(*level, index);
    return ModifyStatus::requeued;
  }

  [[nodiscard]] std::optional<Ticks> best_bid() const noexcept { return best(Side::buy); }

  [[nodiscard]] std::optional<Ticks> best_ask() const noexcept { return best(Side::sell); }

  // Order count resting at a price. Named depth_at for the market depth sense; it counts
  // orders, not shares. total_qty_at reports shares.
  [[nodiscard]] std::uint32_t depth_at(Side side, Ticks price) const noexcept {
    const PriceLevel* const level = find_level(side, price);
    return level == nullptr ? 0U : level->order_count;
  }

  [[nodiscard]] Quantity total_qty_at(Side side, Ticks price) const noexcept {
    const PriceLevel* const level = find_level(side, price);
    return level == nullptr ? Quantity{0} : level->aggregate_qty;
  }

  // Head of the FIFO at a price, for a caller that needs to walk the queue.
  // INVALID_INDEX when the level is empty.
  [[nodiscard]] ArenaIndex first_order_at(Side side, Ticks price) const noexcept {
    const PriceLevel* const level = find_level(side, price);
    return level == nullptr ? INVALID_INDEX : level->head;
  }

  [[nodiscard]] const OrderPool& pool() const noexcept { return pool_; }

  [[nodiscard]] OrderPool& pool() noexcept { return pool_; }

  [[nodiscard]] const OrderIdMap& id_map() const noexcept { return id_map_; }

  [[nodiscard]] ArenaIndex find_order(OrderId id) const noexcept { return id_map_.find(id); }

  [[nodiscard]] const PriceConfig& price_config() const noexcept { return price_config_; }

  [[nodiscard]] Ticks band_base() const noexcept { return band_base_; }

  [[nodiscard]] bool in_band(Ticks price) const noexcept {
    const std::int64_t slot = static_cast<std::int64_t>(price.raw()) - band_base_.raw();
    return slot >= 0 && std::cmp_less(slot, BandLevels);
  }

  // -------------------------------------------------------------------------
  // Instrumentation. Read by tests and by the benchmark report so that published
  // numbers arrive with the conditions that produced them.
  // -------------------------------------------------------------------------

  [[nodiscard]] std::uint64_t rebase_count() const noexcept { return rebase_count_; }

  // Rebases abandoned by the feasibility check. A non-zero value means the cold
  // cap is too small for the price distribution being replayed, which is a sizing
  // problem rather than a bug, and it belongs in the benchmark report.
  [[nodiscard]] std::uint64_t rebase_skipped_count() const noexcept { return rebase_skipped_; }

  [[nodiscard]] std::uint64_t cold_operation_count() const noexcept { return cold_operations_; }

  [[nodiscard]] std::size_t cold_level_count() const noexcept {
    return cold_.size(Side::buy) + cold_.size(Side::sell);
  }

  [[nodiscard]] std::size_t occupied_level_count(Side side) const noexcept {
    return occupied_[side_index(side)].popcount();
  }

  [[nodiscard]] std::size_t band_memory_bytes() const noexcept {
    return 2U * BandLevels * sizeof(PriceLevel);
  }

  [[nodiscard]] std::size_t bitmap_memory_bytes() const noexcept {
    return 2U * Bitmap::memory_bytes();
  }

  [[nodiscard]] std::size_t total_memory_bytes() const noexcept {
    return band_memory_bytes() + bitmap_memory_bytes() + pool_.memory_bytes() +
           id_map_.memory_bytes() + (rebase_scratch_.size() * sizeof(std::uint32_t));
  }

 private:
  [[nodiscard]] static Ticks centre_to_base(Ticks centre) noexcept {
    return Ticks{centre.raw() - static_cast<std::int32_t>(BandLevels / 2)};
  }

  [[nodiscard]] std::size_t slot_of(Ticks price) const noexcept {
    assert(in_band(price));
    return static_cast<std::size_t>(price.raw() - band_base_.raw());
  }

  [[nodiscard]] Ticks price_of_slot(std::size_t slot) const noexcept {
    return Ticks{band_base_.raw() + static_cast<std::int32_t>(slot)};
  }

  // -------------------------------------------------------------------------
  // Intrusive list maintenance. Both functions assume the order is already in the
  // arena and that the level belongs to the order's side and price.
  // -------------------------------------------------------------------------

  void link_at_tail(PriceLevel& level, ArenaIndex index) noexcept {
    Order& order = pool_[index];
    order.prev = level.tail;
    order.next = INVALID_INDEX;

    if (level.tail == INVALID_INDEX) {
      assert(level.head == INVALID_INDEX && level.order_count == 0U);
      level.head = index;
    } else {
      pool_[level.tail].next = index;
    }

    level.tail = index;
    ++level.order_count;
    level.aggregate_qty += order.remaining_qty();
  }

  void unlink(PriceLevel& level, ArenaIndex index) noexcept {
    Order& order = pool_[index];
    assert(level.order_count > 0U);

    if (order.prev == INVALID_INDEX) {
      level.head = order.next;
    } else {
      pool_[order.prev].next = order.next;
    }

    if (order.next == INVALID_INDEX) {
      level.tail = order.prev;
    } else {
      pool_[order.next].prev = order.prev;
    }

    order.prev = INVALID_INDEX;
    order.next = INVALID_INDEX;
    --level.order_count;
    level.aggregate_qty -= order.remaining_qty();
  }

  [[nodiscard]] PriceLevel* level_for_order(const Order& order) noexcept {
    if (in_band(order.price)) {
      return &levels_[side_index(order.side)][slot_of(order.price)];
    }
    return cold_.find(order.side, order.price);
  }

  [[nodiscard]] const PriceLevel* find_level(Side side, Ticks price) const noexcept {
    if (in_band(price)) {
      return &levels_[side_index(side)][slot_of(price)];
    }
    return cold_.find(side, price);
  }

  void remove_from_level(ArenaIndex index) noexcept {
    const Order& order = pool_[index];
    const Side side = order.side;
    const Ticks price = order.price;

    if (in_band(price)) {
      const std::size_t slot = slot_of(price);
      PriceLevel& level = levels_[side_index(side)][slot];
      unlink(level, index);
      if (level.empty()) {
        occupied_[side_index(side)].clear(slot);
      }
      return;
    }

    PriceLevel* const level = cold_.find(side, price);
    assert(level != nullptr);
    unlink(*level, index);
    if (level->empty()) {
      cold_.erase(side, price);
      refresh_cold_counts();
    }
    ++cold_operations_;
  }

  [[nodiscard]] std::optional<Ticks> band_best(Side side) const noexcept {
    const Bitmap& bitmap = occupied_[side_index(side)];
    const std::size_t slot = side == Side::buy ? bitmap.highest_set() : bitmap.lowest_set();
    if (slot == Bitmap::NONE) {
      return std::nullopt;
    }
    return price_of_slot(slot);
  }

  [[nodiscard]] std::optional<Ticks> cold_best(Side side) const noexcept {
    Ticks out{};
    if (cold_.best(side, out)) {
      return out;
    }
    return std::nullopt;
  }

  [[nodiscard]] static std::optional<Ticks> better_of(Side side,
                                                      std::optional<Ticks> left,
                                                      std::optional<Ticks> right) noexcept {
    if (!left.has_value()) {
      return right;
    }
    if (!right.has_value()) {
      return left;
    }
    if (side == Side::buy) {
      return *left >= *right ? left : right;
    }
    return *left <= *right ? left : right;
  }

  // A cold level is not necessarily worse than the band's best. An earlier version
  // of this function assumed it was, on the grounds that rebasing keeps the band
  // centred on the market. That assumption is unenforceable: two resting prices
  // further apart than the band is wide cannot both be in the band, and if the
  // better one is the one outside, the band's best is not the book's best. The
  // unit test for the cold path caught it.
  //
  // The cached level count is what keeps the correct version cheap. Cold storage is
  // empty in every normal book, so the common path is one load and a branch that
  // predicts perfectly, and the ordered container is only touched when it actually
  // holds something.
  [[nodiscard]] std::optional<Ticks> best(Side side) const noexcept {
    const std::optional<Ticks> hot = band_best(side);
    if (cold_levels_[side_index(side)] == 0U) {
      return hot;
    }
    return better_of(side, hot, cold_best(side));
  }

  // -------------------------------------------------------------------------
  // Band rebasing. Rare, slow, and required to be exactly correct: it must not
  // lose a single resting order.
  //
  // It is only ever reached from add and from the cold insert path, after the
  // level mutation for that operation is complete. Nothing calls it partway
  // through a level walk, which is what keeps the Phase 2 rule that a rebase never
  // happens in the middle of a match structurally true rather than merely
  // intended.
  // -------------------------------------------------------------------------

  void maybe_rebase(std::size_t touched_slot) noexcept {
    const bool near_edge =
        touched_slot < REBASE_MARGIN || touched_slot >= BandLevels - REBASE_MARGIN;
    if (!near_edge && !cold_pending_) {
      return;
    }
    rebase_now();
  }

  void rebase_now() noexcept {
    // Cleared unconditionally, even when the rebase turns out to be futile. A
    // cold level too far away to ever fit would otherwise make every subsequent
    // add retry the same recentring calculation forever.
    cold_pending_ = false;

    const std::optional<Ticks> centre = market_centre();
    if (!centre.has_value()) {
      return;
    }

    const Ticks target = centre_to_base(*centre);
    if (target == band_base_) {
      return;
    }
    rebase_to(target);
  }

  // Centres on the true best of each side, cold storage included. Using only the
  // band's best would leave the band stranded when the market gaps past its edge,
  // which is exactly the situation rebasing exists to handle.
  [[nodiscard]] std::optional<Ticks> market_centre() const noexcept {
    const std::optional<Ticks> bid = best(Side::buy);
    const std::optional<Ticks> ask = best(Side::sell);

    if (bid.has_value() && ask.has_value()) {
      return Ticks{
          static_cast<std::int32_t>((static_cast<std::int64_t>(bid->raw()) + ask->raw()) / 2)};
    }
    if (bid.has_value()) {
      return bid;
    }
    return ask;
  }

  // A rebase either completes in full or does not happen. There is no half shifted
  // state, because a level left behind while the band moves under it would answer
  // to the wrong price, and every order in it would be silently misfiled.
  //
  // The feasibility check is what buys that guarantee. Evicting a level to cold can
  // fail when the cold cap is full, and discovering that partway through the shift
  // leaves no correct recovery: the orders cannot be dropped and the band cannot be
  // half moved. So the cost is counted first and the whole rebase is abandoned if
  // it will not fit.
  void rebase_to(Ticks new_base) noexcept {
    const std::int64_t shift = static_cast<std::int64_t>(new_base.raw()) - band_base_.raw();
    assert(shift != 0);

    if (!rebase_is_feasible(shift, new_base)) {
      ++rebase_skipped_;
      return;
    }

    for (std::size_t side = 0; side < 2; ++side) {
      shift_side(static_cast<Side>(side), shift, new_base);
    }

    band_base_ = new_base;
    ++rebase_count_;
    refresh_cold_counts();

    // Deliberately not re-armed from whatever remains in cold storage. A level too
    // far away to ever fit inside the band would otherwise make every following
    // add recompute the same futile recentring. Anything still cold comes back on
    // the next rebase, which the edge margin will trigger when the market returns.
    cold_pending_ = false;
  }

  [[nodiscard]] bool rebase_is_feasible(std::int64_t shift, Ticks new_base) const noexcept {
    const Ticks low = new_base;
    const Ticks high{new_base.raw() + static_cast<std::int32_t>(BandLevels - 1U)};

    for (std::size_t index = 0; index < 2; ++index) {
      const Side side = static_cast<Side>(index);

      const std::size_t evictions = count_evictions(side, shift);
      if (evictions == 0) {
        continue;
      }

      // Reclaiming happens before evicting, so the range about to be pulled out of
      // cold storage counts as capacity the eviction can use.
      const std::size_t reclaims = cold_.count_in_range(side, low, high);
      const std::size_t occupied_after_reclaim = cold_.size(side) - reclaims;
      if (occupied_after_reclaim + evictions > max_cold_levels_) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::size_t count_evictions(Side side, std::int64_t shift) const noexcept {
    const Bitmap& bitmap = occupied_[side_index(side)];

    std::size_t evictions = 0;
    for (std::size_t slot = bitmap.next_set(0); slot != Bitmap::NONE;) {
      const std::int64_t target = static_cast<std::int64_t>(slot) - shift;
      if (target < 0 || std::cmp_greater_equal(target, BandLevels)) {
        ++evictions;
      }
      if (slot + 1U >= BandLevels) {
        break;
      }
      slot = bitmap.next_set(slot + 1U);
    }
    return evictions;
  }

  void refresh_cold_counts() noexcept {
    cold_levels_[side_index(Side::buy)] = cold_.size(Side::buy);
    cold_levels_[side_index(Side::sell)] = cold_.size(Side::sell);
  }

  void shift_side(Side side, std::int64_t shift, Ticks new_base) noexcept {
    const std::size_t si = side_index(side);
    std::vector<PriceLevel>& levels = levels_[si];
    Bitmap& bitmap = occupied_[si];

    // Snapshot occupancy before mutating anything. The cost is proportional to the
    // number of occupied levels, which is hundreds in a real book, not to the
    // 65536 slots in the band. Rebasing a nearly empty book is nearly free, and
    // that is the common case: the band drifts long before it fills.
    std::size_t occupied_count = 0;
    for (std::size_t slot = bitmap.next_set(0); slot != Bitmap::NONE;) {
      rebase_scratch_[occupied_count] = static_cast<std::uint32_t>(slot);
      ++occupied_count;
      if (slot + 1U >= BandLevels) {
        break;
      }
      slot = bitmap.next_set(slot + 1U);
    }

    bitmap.clear_all();

    // Reclaim before evicting, not after. The levels about to come back from cold
    // storage are freeing the very capacity that the evictions below need, and
    // doing it the other way round makes an eviction fail against a cold cap that
    // is about to have room. They are staged in scratch rather than placed
    // immediately because their destination slots are still occupied by levels the
    // relocation loop has not moved yet.
    cold_scratch_.clear();
    cold_.extract_range(side,
                        new_base,
                        Ticks{new_base.raw() + static_cast<std::int32_t>(BandLevels - 1U)},
                        cold_scratch_);

    // Direction matters. Every level moves by the same signed offset, so
    // processing sources in the direction of travel guarantees a destination is
    // only ever written after its own contents have already been moved out.
    if (shift > 0) {
      for (std::size_t i = 0; i < occupied_count; ++i) {
        relocate_level(side, levels, bitmap, rebase_scratch_[i], shift);
      }
    } else {
      for (std::size_t i = occupied_count; i-- > 0;) {
        relocate_level(side, levels, bitmap, rebase_scratch_[i], shift);
      }
    }

    // A cold price was outside the old band by definition, so it cannot collide
    // with a level the loop above just shifted. The assert states that rather than
    // leaving it as a comment.
    for (const std::pair<Ticks, PriceLevel>& entry : cold_scratch_) {
      const auto slot = static_cast<std::size_t>(entry.first.raw() - new_base.raw());
      assert(slot < BandLevels);
      assert(levels[slot].empty());
      levels[slot] = entry.second;
      bitmap.set(slot);
      ++cold_operations_;
    }
  }

  void relocate_level(Side side,
                      std::vector<PriceLevel>& levels,
                      Bitmap& bitmap,
                      std::uint32_t from_slot,
                      std::int64_t shift) noexcept {
    const std::int64_t target = static_cast<std::int64_t>(from_slot) - shift;
    PriceLevel& source = levels[from_slot];

    if (target >= 0 && std::cmp_less(target, BandLevels)) {
      levels[static_cast<std::size_t>(target)] = source;
      source.reset();
      bitmap.set(static_cast<std::size_t>(target));
      return;
    }

    // Falls outside the new band. Orders hold absolute tick prices, so the level
    // moves but not one order field changes, which is the payoff for storing an
    // absolute price on the order rather than a band relative slot.
    const Ticks price = price_of_slot(from_slot);
    PriceLevel* const cold = cold_.level_for(side, price);

    // rebase_is_feasible counted exactly these evictions against the cold cap
    // before any state was touched, so a failure here is impossible unless that
    // accounting and this loop have drifted apart.
    assert(cold != nullptr && "eviction exceeded the capacity the feasibility check reserved");

    *cold = source;
    source.reset();
    ++cold_operations_;
  }

  OrderPool pool_;
  OrderIdMap id_map_;
  ColdLevels cold_;
  PriceConfig price_config_;
  Ticks band_base_;
  std::size_t max_cold_levels_;

  std::array<std::vector<PriceLevel>, 2> levels_{};
  std::array<Bitmap, 2> occupied_{};

  // Mirror of cold_.size(side), cached so that best() can skip the ordered
  // container with a single load in the overwhelmingly common case where nothing
  // is cold. Refreshed after every cold mutation rather than incremented at each
  // site, because there are five such sites and one of them is a rebase loop.
  std::array<std::size_t, 2> cold_levels_{};

  // Rebase working storage, sized once at construction so that rebasing itself
  // performs no allocation beyond what the cold container needs.
  std::vector<std::uint32_t> rebase_scratch_;
  std::vector<std::pair<Ticks, PriceLevel>> cold_scratch_;

  bool cold_pending_ = false;
  std::uint64_t rebase_count_ = 0;
  std::uint64_t rebase_skipped_ = 0;
  std::uint64_t cold_operations_ = 0;
};

}  // namespace ob
