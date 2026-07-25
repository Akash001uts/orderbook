# Design

Each section states a decision, the alternatives considered, and why the decision
was taken. Sections covering components that are not yet built are absent rather
than stubbed: this document tracks what exists.

## Contents

- [Determinism as a design constraint](#determinism-as-a-design-constraint)
- [Strong typedefs over bare integers](#strong-typedefs-over-bare-integers)
- [Fixed point prices and the tick coordinate](#fixed-point-prices-and-the-tick-coordinate)
- [Order layout and why it is 40 bytes](#order-layout-and-why-it-is-40-bytes)
- [PriceLevel layout and the 24 versus 32 byte question](#pricelevel-layout-and-the-24-versus-32-byte-question)
- [Events as data, not callbacks](#events-as-data-not-callbacks)
- [Non-atomic event ring](#non-atomic-event-ring)
- [Dependency justifications](#dependency-justifications)
- [Warning flags on an interface target](#warning-flags-on-an-interface-target)
- [Out of scope, and why](#out-of-scope-and-why)

## Determinism as a design constraint

**Decision.** The engine reads no wall clock, consumes no randomness, performs no
I/O, and throws no exceptions. Time enters only as the `timestamp` field on
`ob::Command`.

**Alternative considered.** Let the engine stamp arrival times itself with
`std::chrono::steady_clock`. This is what most textbook implementations do and it
is simpler at the call site.

**Reasoning.** A clock read is a syscall or at best a `vDSO` call plus a fence,
which is measurable against a target of tens of nanoseconds per operation. That
is the performance argument, and it is the weaker of the two. The real argument is
testability: with an internal clock, two runs of the same command sequence produce
different state, so "the fast implementation and the naive implementation agree"
stops being a checkable claim. The differential test in Phase 2 asserts byte
identical state after every single command across millions of commands. That test
is the strongest correctness evidence in the repository and it is only possible
because the engine is a pure function of its input sequence.

**Cost accepted.** Callers must supply timestamps, so the replay driver and the
strategy driver each carry a notion of current time that the engine does not.

## Strong typedefs over bare integers

**Decision.** `Price`, `Ticks`, `Quantity`, `OrderId`, `Timestamp`, and `Sequence`
are distinct types built on a `StrongInt<Rep, Tag>` template. Construction from
the representation is explicit, extraction requires a named `raw()` call, and only
dimensionally meaningful operators exist: same-type addition and subtraction, and
scaling by a plain integer.

**Alternatives considered.**

1. `using Price = std::int64_t`. Zero friction, zero safety. Every one of these
   compiles: passing a quantity where a price is expected, adding an order id to a
   timestamp, indexing a level array by a raw price rather than a tick offset.
2. `enum class Price : std::int64_t`. Gives distinctness and blocks implicit
   conversion for free, but arithmetic requires casting out and back on every use,
   which produces exactly the `static_cast` noise the type layer is meant to
   remove.

**Reasoning.** The book manipulates six different integer meanings, four of which
are 64-bit unsigned and therefore mutually interchangeable to the compiler. The
mistakes that class of bug produces are silent and they corrupt state rather than
crashing. `test/test_types.cpp` asserts the absence of every cross conversion via
`std::is_convertible_v`, and asserts that cross-type addition is ill formed using
a `requires` expression, since `static_assert` cannot directly express "this
expression must not compile".

**Cost accepted.** `.raw()` appears at every boundary where an integer must leave
the type system: hash computation, array indexing, and serialisation. This is
visible in the code and it is the intended trade. The wrapper itself is free, and
`test_types.cpp` asserts that: `sizeof(Price) == sizeof(std::int64_t)` and the
type is trivially copyable and standard layout.

## Fixed point prices and the tick coordinate

**Decision.** Two separate price types. `Price` is the venue price in scaled
integer units, four implied decimals to match ITCH 5.0. `Ticks` is a signed 32-bit
tick index, the offset of a price from the band base measured in whole `tick_size`
steps. `PriceConfig` converts between them and conversion happens only at the API
boundary.

**Alternative considered.** A single price type, with the level array indexed by
`price / tick_size` computed at each use.

**Reasoning.** Floating point is disqualified outright: `0.01` is not
representable in binary, so price comparison and aggregation both accumulate
error, and two books that should agree exactly will not. Given integers, the
question is where the division lives. A direct-indexed level array needs a small
dense index, which means a division and a subtraction per lookup if the book
stores venue prices. Converting once on command entry moves that division off the
hot path entirely: inside the book every price is already an array index.

`Ticks` is signed rather than unsigned because band rebasing and band-relative
arithmetic both produce transient negatives, and an unsigned type would turn those
into enormous positive indices, which is the exact bug class that a bounds assert
would catch only in debug.

**Cost accepted.** Prices not on a tick boundary must be rejected at the boundary,
so `PriceConfig::on_tick_boundary` exists and callers must use it. The engine
returns `RejectReason::off_tick` rather than silently rounding, because silent
rounding changes a customer's price.

## Order layout and why it is 40 bytes

**Decision.** `sizeof(Order) == 40`, asserted in the header. Neighbours are
32-bit arena indices, not pointers. Field order groups the cancel path fields
together.

```
OrderId       id           8   venue order reference number
Timestamp     timestamp    8   nanoseconds since midnight, from the input event
ArenaIndex    prev         4   INVALID_INDEX at the level head
ArenaIndex    next         4   INVALID_INDEX at the level tail
Ticks         price        4   the level this order belongs to
uint32        remaining    4   shares, ITCH's own field width
uint32        generation   4   use after free detection
ParticipantId party        2   self trade prevention
Side          side         1
                           1   tail padding
```

**Alternatives considered.**

1. Pointers instead of indices. Natural in C++, and 16 bytes wider per order.
2. `std::list<Order>` per price level. One allocation per order, nodes scattered
   across the heap, and a pointer chase per hop with no locality.
3. Golf the struct to 32 bytes by dropping `generation` and `party`, giving exactly
   two orders per cache line.

**Reasoning on indices.** A 32-bit index is half the width of a pointer, so twice
as many links fit in a cache line, and the whole arena becomes relocatable: it can
be `memcpy`'d, snapshotted, or grown without fixing up a single link. That
relocatability is what makes band rebasing tractable. The cost is one addition
against the arena base per dereference, which is a single instruction against a
value already in a register.

**Reasoning on 40 versus 32.** The 32-byte variant is genuinely attractive and it
was measured against. It requires dropping the generation counter, which is the
only mechanism that catches a stale arena index being dereferenced after its slot
has been recycled. That is the single most likely bug class in an arena based
design and it is silent without the counter. The 8 extra bytes buy a hard failure
in debug instead of corrupted state in production, so 40 stands. If Phase 4
measurements show the arena working set is the binding constraint, the counter can
move to a parallel array compiled only in debug, at the cost of the next point.

**Reasoning on keeping the counter in release builds.** The counter is present in
every configuration, not only debug, so that the debug and release layouts are
byte identical. If they differed, the differential test, which runs in debug,
would prove nothing about the release binary the benchmarks measure. Paying 8
bytes to keep those two claims about the same object is worth it.

**Per-order shares are 32-bit** while aggregates are 64-bit. The narrow field
matches the width of the ITCH 5.0 shares field, so no venue message can overflow
it, while level and book aggregates sum many orders and need the headroom.
`MAX_ORDER_SHARES` marks the one place where a caller supplied `Quantity` narrows.

## PriceLevel layout and the 24 versus 32 byte question

**Decision.** `sizeof(PriceLevel) == 24`: a 64-bit aggregate quantity, head and
tail arena indices, and a 32-bit order count.

**Alternatives considered.**

1. Pad to 32 bytes so that levels never straddle a cache line boundary and two fit
   per line exactly.
2. Shrink the aggregate to 32 bits, giving exactly 16 bytes and four levels per
   line.
3. Split into a structure of arrays: one array of aggregate quantities, one array
   of list heads and tails.

**Reasoning.** At 24 bytes, 2.67 levels fit in a 64-byte line and some levels
straddle. Padding to 32 removes the straddle but costs 33 percent more memory
across a 65536 level band per side, which moves the level array's resident set
from roughly 1.5 MiB per side to 2 MiB per side and pushes it further out of L2. An
aggressive order walking outward from the best price touches consecutive levels,
so the denser layout wins more from prefetch than it loses to the occasional extra
line touch. The 16-byte variant was rejected because a 32-bit aggregate can
overflow when many orders rest at one price, and an overflowing aggregate is a
silent correctness failure rather than a performance one.

The structure of arrays split is the strongest of the alternatives and is not
ruled out. `best_bid`, `best_ask`, and `total_qty_at` read only the aggregate, so
splitting would halve the bytes touched by every depth query. It is deferred
because it doubles the number of index computations in the matching loop, which
reads both halves, and that trade needs Phase 4 numbers to settle rather than an
argument. This is recorded here so the omission is a decision rather than an
oversight.

**Invariant coupling.** `order_count == 0` is mirrored by the level bitmap.
Anything that takes a level from empty to occupied, or back, must update both
structures or `best_bid` will report a price with no liquidity behind it. This is
the sharpest edge in the Phase 1 design and it is why the bitmap is updated only
on empty and fill transitions rather than on every quantity change: fewer update
sites means fewer places to get it wrong, and quantity changes do not affect
occupancy.

## Events as data, not callbacks

**Decision.** The engine appends `ExecutionEvent` structs to a caller provided
ring. It never writes to stdout, never logs, never calls back into user code.

**Alternatives considered.** A `std::function` callback per fill, or a virtual
`EventSink` interface.

**Reasoning.** Both alternatives are banned on the hot path for the same reason:
they are indirect calls the compiler cannot inline or devirtualise, so every fill
pays a pipeline hazard and the matching loop cannot be optimised across the call.
`std::function` additionally may heap allocate on construction. Writing a 56-byte
struct into a preallocated array is a sequential store the hardware prefetcher
already anticipates. It also decouples the engine from what the consumer wants:
the latency harness, the differential test, and the strategy driver each read the
same event stream and do entirely different things with it.

**Cost accepted.** The consumer must drain the ring. If it does not, events are
dropped, so `overflowed()` exists and tests assert it stays false. A dropped event
would desynchronise the differential comparison, which makes ring capacity a
correctness parameter and not just a tuning knob.

## Non-atomic event ring

**Decision.** `EventRing` uses plain `std::size_t` read and write indices, not
atomics. Capacity is a power of two so the wrap is a mask.

**Alternative considered.** An atomic single-producer single-consumer ring with
acquire and release ordering, which would allow a consumer on another thread.

**Reasoning.** The engine and its consumer run on the same thread in every use
this project has. Making the indices atomic would put a lock-prefixed
read-modify-write on the hot path to buy a cross-thread guarantee that nothing
needs. Cross-thread event consumption is a real design, but it belongs with
multi-symbol sharding, which is explicitly out of scope. If that scope changed,
this is the type to change and the change is local.

## Dependency justifications

The global constraint is that dependencies stay minimal and vendored through CMake
`FetchContent`, with anything beyond the sanctioned set justified here.

| Dependency | Version | Why |
| --- | --- | --- |
| GoogleTest | v1.17.0 | Unit and differential tests. Chosen over Catch2 for its parameterised test support, which the differential test uses to run many seeds as separately reported cases. |
| Google Benchmark | v1.9.4 | Microbenchmarks. It handles iteration count selection, `DoNotOptimize` barriers, and per-iteration statistics, all of which are easy to get subtly wrong by hand. |

HdrHistogram_c is added in Phase 4, where the
latency harness first needs it. It is not fetched before then, because an unused
dependency is still a dependency someone has to build.

Nothing else is fetched.

## Warning flags on an interface target

**Decision.** `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wold-style-cast -Werror` live on an `ob_flags` INTERFACE target that only this
project's targets link, rather than in `CMAKE_CXX_FLAGS`.

**Reasoning.** GoogleTest and Google Benchmark do not compile clean under
`-Wconversion` and `-Wold-style-cast`, and their warnings are not ours to fix.
Putting the flags in `CMAKE_CXX_FLAGS` would apply them to fetched dependencies
and force either a `-Werror` removal or a pile of suppressions. Scoping them to an
interface target keeps `-Werror` genuinely strict on the code that matters.
Sanitizer flags do go in `CMAKE_CXX_FLAGS` via the presets, deliberately, because
AddressSanitizer needs the entire binary instrumented to be sound.

`-Wconversion` and `-Wsign-conversion` are the two that matter most here. This
codebase narrows 64-bit quantities to 32-bit order fields and mixes signed tick
offsets with unsigned array indices, which is precisely where those warnings pay.

## Out of scope, and why

Each of the following was considered and excluded. The common thread is that each
multiplies the surface area without adding evidence the current scope does not
already provide.

**Network transport, FIX, and SBE gateways.** A gateway is a serialisation and
session-management problem, not a matching problem. It would add a large amount of
protocol code and a network stack to tune, and the interesting latency questions
would move from the book to the NIC. The ITCH parser already demonstrates
binary wire format decoding, including the unaligned big-endian access that is the
genuinely hard part.

**Multi-symbol thread sharding.** Real engines shard by symbol across cores, and
the design is well understood: each shard owns its symbols and there is no shared
mutable state. Because there is no cross-shard interaction, adding it multiplies
the harness and lifecycle code without exercising a new idea in the book itself.
Single symbol keeps the measurement clean, since a sharded throughput number is a
statement about the scheduler as much as about the book.

**Persistence and a write-ahead log.** Durability changes the hot path
fundamentally: every mutation becomes a write plus a barrier, and the design
question becomes group commit and fsync batching. That is a storage engine
project, and it would obscure the in-memory data structure work.

**Clustering and replicated matching.** Consensus over an ordered command log is
a distributed systems project.

**A web UI.** No engineering signal.

**A live trading connection.** Requires venue credentials and real capital, and
adds risk without adding evidence.
