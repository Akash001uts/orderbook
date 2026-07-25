#pragma once

#include <cstdint>

// Global allocation counter, hooked through a replacement of operator new.
//
// This is what turns "the hot path does not allocate" from a claim in a design
// document into an assertion a test can fail on. Replacing the global operator
// new means every allocation in the test binary is counted, including any that
// arrives from inside the standard library, so a test measures a region by
// resetting the counter immediately before it.
//
// Coverage note: the plain and array forms of operator new and operator delete
// are replaced, including the sized and nothrow overloads. The over-aligned
// forms, operator new(size_t, align_val_t) and friends, are not. They are only
// reachable for types whose alignment exceeds max_align_t, and test_book.cpp
// carries static_asserts proving that no type this book allocates is in that
// category. If one of those assertions ever fires, the over-aligned overloads
// need replacing too or the counter will silently undercount.
//
// ThreadSanitizer note: tsan's runtime defines its own operator new and delete as
// strong symbols, so a replacement in this file is a duplicate definition and the
// link fails. AddressSanitizer does not have this problem because its versions are
// weak. Under tsan the replacements are therefore compiled out and
// counting_is_active returns false, so that tests which depend on the counter skip
// with a reason rather than passing vacuously against a counter stuck at zero.
namespace ob::testing {

struct AllocationStats {
  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::uint64_t bytes = 0;
};

[[nodiscard]] AllocationStats allocation_stats() noexcept;

void reset_allocation_stats() noexcept;

// False when the operator new replacement was compiled out, which currently means
// a ThreadSanitizer build. A test that asserts on allocation counts must check
// this and skip, because otherwise it compares a counter that never moves against
// an expectation of zero and passes for the wrong reason.
[[nodiscard]] bool counting_is_active() noexcept;

// Scoped reset, so a measured region cannot forget to establish its baseline.
class AllocationGuard {
 public:
  AllocationGuard() noexcept { reset_allocation_stats(); }

  AllocationGuard(const AllocationGuard&) = delete;
  AllocationGuard& operator=(const AllocationGuard&) = delete;
  AllocationGuard(AllocationGuard&&) = delete;
  AllocationGuard& operator=(AllocationGuard&&) = delete;

  ~AllocationGuard() = default;

  [[nodiscard]] std::uint64_t allocations() const noexcept {
    return allocation_stats().allocations;
  }

  [[nodiscard]] std::uint64_t bytes() const noexcept { return allocation_stats().bytes; }
};

}  // namespace ob::testing
