#include "alloc_counter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

// ThreadSanitizer's runtime defines operator new and delete as strong symbols, so
// replacing them here makes the link fail with a duplicate definition. There is no
// way to have both, so under tsan the replacements are omitted and the counter is
// reported inactive. AddressSanitizer's versions are weak, so asan keeps full
// coverage and remains the sanitizer that actually exercises this file.
// NOLINTBEGIN(cppcoreguidelines-macro-usage) a constexpr constant cannot drive
// #if, and whether these definitions exist at all is the thing being decided.
#ifdef __SANITIZE_THREAD__
#define OB_ALLOC_COUNTER_ENABLED 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OB_ALLOC_COUNTER_ENABLED 0
#endif
#endif

#ifndef OB_ALLOC_COUNTER_ENABLED
#define OB_ALLOC_COUNTER_ENABLED 1
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

// Replacing the global operator new means doing three things clang-tidy exists to
// discourage: keeping mutable global state, calling malloc and free directly, and
// handing back raw owning pointers. There is no version of this file that avoids
// them. A replacement operator new cannot allocate through anything higher level,
// because everything higher level allocates through it, and the counter it
// maintains is global because the thing it counts is global.
//
// The suppression is scoped to this file rather than switched off in .clang-tidy,
// so the checks keep their teeth everywhere else.
//
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-non-const-global-variables)

namespace {

// Not atomic. Every test that reads these runs single threaded, and making them
// atomic would let a counter update perturb the very cache behaviour some of
// these tests exist to characterise.
std::uint64_t g_allocations = 0;
std::uint64_t g_deallocations = 0;
std::uint64_t g_bytes = 0;

#if OB_ALLOC_COUNTER_ENABLED

// malloc(0) may legally return nullptr, which operator new must not do, so a
// zero sized request is rounded up to one byte.
void* counted_malloc(std::size_t size) noexcept {
  ++g_allocations;
  g_bytes += size;
  return std::malloc(size == 0 ? 1 : size);
}

void counted_free(void* memory) noexcept {
  if (memory != nullptr) {
    ++g_deallocations;
  }
  std::free(memory);
}

#endif  // OB_ALLOC_COUNTER_ENABLED

}  // namespace

namespace ob::testing {

AllocationStats allocation_stats() noexcept {
  return AllocationStats{
      .allocations = g_allocations, .deallocations = g_deallocations, .bytes = g_bytes};
}

void reset_allocation_stats() noexcept {
  g_allocations = 0;
  g_deallocations = 0;
  g_bytes = 0;
}

bool counting_is_active() noexcept {
  return OB_ALLOC_COUNTER_ENABLED != 0;
}

}  // namespace ob::testing

#if OB_ALLOC_COUNTER_ENABLED

void* operator new(std::size_t size) {
  void* const memory = counted_malloc(size);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  return memory;
}

void* operator new[](std::size_t size) {
  void* const memory = counted_malloc(size);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  return memory;
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return counted_malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return counted_malloc(size);
}

void operator delete(void* memory) noexcept {
  counted_free(memory);
}

void operator delete[](void* memory) noexcept {
  counted_free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
  counted_free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
  counted_free(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
  counted_free(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
  counted_free(memory);
}

#endif  // OB_ALLOC_COUNTER_ENABLED

// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,cppcoreguidelines-avoid-non-const-global-variables)
