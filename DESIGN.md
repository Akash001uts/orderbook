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
- [Open addressing over std::unordered_map](#open-addressing-over-stdunorderedmap)
- [Hierarchical bitmaps for best price](#hierarchical-bitmaps-for-best-price)
- [Band rebasing, and what it costs](#band-rebasing-and-what-it-costs)
- [The overflow cold path](#the-overflow-cold-path)
- [Arena capacity is a cache decision](#arena-capacity-is-a-cache-decision)
- [Differential testing, and why it is the strongest evidence here](#differential-testing-and-why-it-is-the-strongest-evidence-here)
- [Self trade prevention as a template parameter](#self-trade-prevention-as-a-template-parameter)
- [Fill-or-kill as a precondition, not an optimisation](#fill-or-kill-as-a-precondition-not-an-optimisation)
- [A market order is a limit at the extreme](#a-market-order-is-a-limit-at-the-extreme)
- [ITCH decoding: byte assembly rather than struct casting](#itch-decoding-byte-assembly-rather-than-struct-casting)
- [Two independent sources of truth about message length](#two-independent-sources-of-truth-about-message-length)
- [Replay reconstructs; it does not re-match](#replay-reconstructs-it-does-not-re-match)
- [Symbol filtering by locate code](#symbol-filtering-by-locate-code)
- [Order replace loses queue priority](#order-replace-loses-queue-priority)
- [Messages that do not touch the book](#messages-that-do-not-touch-the-book)
- [Memory mapping rather than reading](#memory-mapping-rather-than-reading)
- [The synthetic generator, and why it must be semantically valid](#the-synthetic-generator-and-why-it-must-be-semantically-valid)
- [What a real NASDAQ capture confirmed, and what it revealed](#what-a-real-nasdaq-capture-confirmed-and-what-it-revealed)
- [Bugs caught by the tests, and what they teach](#bugs-caught-by-the-tests-and-what-they-teach)
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

## Open addressing over std::unordered_map

**Decision.** A custom open addressing table with linear probing, a power of two
capacity, Fibonacci hashing, and backward shift deletion. Maximum load factor
0.5. Keys and values live in separate arrays.

**Alternatives considered.** `std::unordered_map`, and a tombstone based open
addressing table.

**Reasoning on the container.** `std::unordered_map` is specified as buckets of
nodes, so a lookup dereferences a bucket pointer and then walks a chain of
separately allocated nodes. Cancel is one of the three hot operations and it
begins with exactly one of these lookups. It also allocates on every insert,
which the zero allocation rule forbids outright.

**Reasoning on backward shift.** Tombstones are easier and they are a trap here.
A tombstone keeps counting toward probe length forever, so a book that cancels
heavily, which is every real book, degrades permanently. Backward shift restores
the table to the state it would have had if the key had never been inserted. The
subtlety is that an entry may only move back into the hole if its ideal slot does
not lie cyclically within the range being closed, and
`test_book.cpp: EveryKeyStaysReachableAcrossHeavyChurn` exists because a naive
version of that condition passes every simple test and loses keys under churn.

**Reasoning on the load factor.** For linear probing the expected probe count on a
successful lookup is about `(1 + 1/(1-a)^2)/2`: roughly 2.5 probes at 0.5, 8.5 at
0.75, and 50 at 0.9. The knee is sharp and the memory saved by a denser table is
trivial next to the order arena. Measured at the design point the table reports a
mean probe count of 1.0 and a lookup costs 2.4 ns.

**Reasoning on the structure of arrays split.** Probing reads nothing but keys
until it finds its match. Contiguous 8-byte keys put eight candidates in every
line fetched; interleaving key and value would put four per line and waste half of
each fetch on values belonging to keys that did not match.

**Reasoning on the hash.** Identity masking is tempting because ITCH order
reference numbers arrive nearly sequential, which identity masking would place
with no collisions at all. It fails when the venue's numbering has a stride
sharing a factor with the capacity, which is a property of the feed rather than of
this code, and the failure is silent clustering. One multiply and one shift
removes the dependency on someone else's numbering scheme.

## Hierarchical bitmaps for best price

**Decision.** A three tier occupancy bitmap per side. One bit per level, one bit
per bottom tier word, and a single top word. Best price is a `countr_zero` or
`bit_width` at each tier.

**Alternatives considered.**

1. **Scan outward from the last known best.** O(distance), and the distance is
   unbounded. One large cancel at the touch can leave the next occupied level
   thousands of ticks away, turning a cancel into a scan of tens of kilobytes at
   exactly the moment the book is busiest.
2. **Cache best bid and best ask as values.** O(1) to read, but repairing them
   after the best level empties needs the scan above, so it moves the cost rather
   than removing it, and it adds a second source of truth that can disagree.
3. **A tree or heap keyed by price.** O(log n) with pointer chasing and
   allocation, which is the design this project exists to beat.

**Reasoning.** Three dependent loads and three single cycle instructions, and the
cost does not depend on how far apart the occupied levels are. At the default band
the whole structure is 8 KiB per side, so it stays resident.

**The invariant, and why it is the sharpest edge here.** A bit is set if and only
if the level has a non-zero order count. Occupancy is updated only on empty to
occupied transitions and back, never on a quantity change, which keeps the number
of call sites that can break it to two per side. If it breaks, the symptom is
`best_bid` reporting a price with no liquidity behind it.

## Band rebasing, and what it costs

**Decision.** The band recentres on the market when an operation touches a level
within an eighth of the band of either edge. Rebasing moves only occupied levels,
found by walking the bitmap, so it costs O(occupied levels) rather than O(band).
Orders store absolute tick prices, so a rebase moves levels without touching a
single order field.

**A rebase either completes in full or does not happen.** This is not a stylistic
preference. Evicting a level to the overflow container can fail when the cold cap
is full, and discovering that partway through the shift leaves no correct
recovery: the orders cannot be dropped and the band cannot be left half moved. So
the evictions are counted against the available cold capacity before any state is
touched, and the whole rebase is abandoned if it will not fit. An abandoned rebase
is safe: the band simply stays where it is.

The first implementation did not do this. It handled a failed eviction by leaving
the level where it was and continuing the shift, which silently misfiled every
order in that level, because the level then answered to whatever price its slot
mapped to under the new base. See the bug log below.

**What it costs when it triggers.** A rebase is a scan of the occupancy bitmap
plus a copy of each occupied level, plus an ordered container operation for every
level that crosses the boundary in either direction. It is far more expensive than
any hot operation and it will be visible in the p99.9 and p99.99 tail. The margin
is set at an eighth of the band, roughly 8192 ticks at the default size, so that a
normal session never reaches it.

## The overflow cold path

**Decision.** Prices outside the band go to a `std::map` keyed by tick, capped at a
configurable number of levels per side, hidden behind a pointer so that `<map>`
never enters a hot header.

**Reasoning.** The band cannot be unbounded and a price a million ticks away has to
go somewhere. Making it slow and correct is better than rejecting it. The cap is
what keeps the memory bounded and known at construction rather than unbounded at
runtime; beyond it, an add is rejected with `band_overflow`.

**A cold level is not necessarily worse than the band's best.** The first version
assumed it was, reasoning that rebasing keeps the band centred on the market. That
assumption is unenforceable: two resting prices further apart than the band is wide
cannot both be in the band, and if the better one is outside then the band's best
is not the book's best. `best()` now compares both, guarded by a cached count so
that the ordered container is untouched when nothing is cold. See the bug log.

**On terminating rather than returning an error.** The cold path allocates, so it
can in principle throw `bad_alloc`, and the functions that reach it are marked
`noexcept`, which turns that into a terminate. This is deliberate. An order book
that cannot store an order has no correct alternative to failing loudly, and the
condition means the machine is out of memory, not that the caller did anything a
caller could handle.

## Arena capacity is a cache decision

Every arena slot is 40 bytes and the id map adds another 24 per slot at the design
load factor. So 2^16 orders costs roughly 4 MiB across the two, and 2^20 costs
roughly 64 MiB.

Measured on a machine with 2.5 MiB L2 and 8 MiB L3, an add costs about 18 ns while
that working set fits in L2 and about 130 ns when it does not, with the algorithmic
work identical at every point. Oversizing the arena is therefore not free headroom,
it is a seven times slowdown on every operation. The default is sized to a busy
single symbol rather than to the largest book imaginable. The curve is published in
BENCHMARKS.md.

## Differential testing, and why it is the strongest evidence here

**Decision.** Every randomised command goes to both the real engine and a
deliberately naive `std::map` plus `std::list` reference book, and full state
equivalence is asserted after every single command: best bid, best ask, per level
aggregate quantity, per level order count, the exact ordered sequence of order ids
at every occupied level, and the emitted event stream.

**Why after every command rather than at the end.** A comparison at the end tells
you something diverged somewhere in a million commands, which is nearly useless
for debugging. A comparison after each one names the exact command, with a book
small enough to read.

**Why the queue order check matters most.** Aggregates can agree while queue order
is wrong. Queue order is what price time priority actually promises, so comparing
the id sequence at every level is the check that tests the promise rather than a
proxy for it. It is also the check that caught the requeue mutation below.

**The known limitation, stated rather than glossed.** Both implementations emit
the same event shapes, because the event protocol is a design decision rather than
a derived fact. A misconception about what an event should contain would be shared
by both and the comparison would not see it. That is why `test/test_engine.cpp`
exists: those tests are written from the specification text, not from either
implementation, and they cover the semantics the differential test cannot
independently confirm.

### Validating the test by mutation

A differential test that has never been observed to fail is a differential test
nobody should trust. Three bugs were deliberately injected and each was caught by
a different one of the comparison's checks:

| Injected bug | Caught by | At command |
| --- | --- | --- |
| Crossing predicate changed from `<=` to `<` | event stream | 3 |
| Partial fill skips the level aggregate update | aggregate quantity | 9 |
| A quantity reduction requeues instead of holding its place | queue order | 399 |

Each check earns its place, which is the point of running the exercise rather than
assuming it.

### Shrinking and regressions

On mismatch the failing sequence is reduced by repeated delta debugging passes:
drop a contiguous chunk, keep the drop if the failure survives, halve the chunk
when a pass stops making progress. Commands are not independent, since a cancel
refers to an earlier add, so many candidate drops change the failure rather than
preserving it and are rejected by re-running. The result is written to
`test/regressions/` in a plain text format and replayed on every subsequent run.

That is what makes the reduced push budget safe. A bug found once at any budget is
pinned by a deterministic test from then on, so the nightly ten million command
run explores rarer state rather than standing between a regression and `main`.

## Self trade prevention as a template parameter

**Decision.** The policy is a template parameter, and two policies exist:
`CancelNewest`, the venue default, and `CancelOldest`.

**Why a template rather than a runtime flag.** The policy is consulted on every
fill. A runtime branch there would be an unpredictable test in the hottest loop
the engine has. As a template parameter it is a compile time constant, so the
check folds away entirely for the overwhelmingly common case where the two
participants differ.

**Why two policies rather than one.** A template parameter with a single
instantiation is not extensibility, it is the appearance of it. The second
implementation is what proves the seam is real and that the engine handles both
outcomes, and both are covered by the differential test.

**Not implemented, and why.** Cancel-both removes both sides, which is simple but
punishes a resting order that did nothing wrong. Decrement-and-cancel reduces both
by the overlap, which is what CME uses and which needs the aggressor's original
size threaded through the match loop. Neither demonstrates a mechanism this pair
does not already demonstrate.

## Fill-or-kill as a precondition, not an optimisation

Fill-or-kill totals the reachable liquidity before touching any state, and rejects
without mutating if it falls short. This is the one order type where a partial
mutation would be observably wrong.

The subtlety is that the total must exclude size the aggressor cannot legally
trade against. Orders that self trade prevention would block are skipped, and
under cancel-newest the walk stops entirely at the first such order, because the
aggressor would stop dead there too. Counting unreachable size would let a
fill-or-kill pass its check and then fail to fill, which is the exact outcome the
check exists to prevent.

**The mirror image is just as wrong, and it is the bug this section originally
missed.** Counting *less* than the matcher can reach makes the check reject an
order the matcher would have filled in full. That is conservative in direction, so
it is easy to overlook, but it is still the precheck and the matcher disagreeing
about the same book. The walk must therefore see exactly what the matcher sees,
which means both must traverse the union of the band and cold storage. See bug 8 in
the log below: the two used to disagree, and every differential configuration in
the suite was blind to it.

The general rule this leaves behind: any precheck that predicts what a mutating
path will do has to enumerate over precisely the same structures, or it is
predicting a different book.

## A market order is a limit at the extreme

Rather than a separate matching path, a market order is given a synthetic limit at
the most aggressive expressible price, so one loop serves every order type. Two
matching paths would be two places for the semantics to drift apart.

The sentinel does not leak into the event stream: a market order's events report a
price of zero, because a market order has no price and a reader looking at that
field would otherwise see an internal constant.

## ITCH decoding: byte assembly rather than struct casting

**Decision.** Every field is assembled from individual bytes by explicit big endian
load helpers. No pointer into the buffer is ever cast to a struct or to a wider
integer type.

**Alternative considered.** Define a packed struct per message type and
`reinterpret_cast` the buffer to it, then byte swap the fields.

**Reasoning.** The shortcut is undefined behaviour twice over. It violates strict
aliasing, and ITCH fields are unaligned: an eight byte order reference number sits
at offset 11 of every order message. On x86 the unaligned load happens to work,
which is precisely what makes it dangerous, because the code looks correct until it
is compiled for a different target or run under UndefinedBehaviorSanitizer. This
was the parser rule I was most careful about.

The byte assembly costs nothing. Both GCC and Clang recognise the pattern and emit
a single load plus a `bswap` at `-O2` and above, which the measured 312 million
messages per second on the parse-only path confirms.

**The six byte timestamp** is the field that makes struct casting impossible
anyway. There is no 48-bit integer type, so it has to be assembled into a `uint64`
with the top two bytes left clear regardless of approach.

## Two independent sources of truth about message length

**Decision.** The parser carries a length table for every known type *and* reads
the two byte length prefix that the sample files carry, then checks them against
each other. A disagreement aborts the parse.

**Reasoning.** They serve different purposes. The prefix is what lets an
*unrecognised* type be skipped correctly, which is the property that keeps an
unknown message from desynchronising the whole remainder of the stream. The table
is what detects corruption and version drift, because a prefix that disagrees with
the known length of a known type means either the bytes are damaged or this build's
assumptions about the protocol are wrong. Continuing from a plausible looking
offset in either case produces confident nonsense, which is worse than stopping.

## Replay reconstructs; it does not re-match

**Decision.** The replay driver applies ITCH messages directly to the book. It
does not feed historical orders through the matching engine.

**Reasoning, and this is the most important decision in the ITCH layer.** An
execution message is the venue telling us a trade has already happened. The correct
response is to reduce the resting order by the executed size. Feeding historical
orders through the matcher instead would re-derive trades the stream has already
reported and double count every one of them.

Phase 5's strategy orders are the opposite case. Those are hypothetical, no venue
has ever matched them, so they do go through the engine and contend for real queue
position against this reconstructed book. The same `Book` serves both, which is what
makes the strategy's fills interact with real queue dynamics rather than with a
separate optimistic model.

## Symbol filtering by locate code

**Decision.** Filtering is on the two byte stock locate code from the message
header, resolved by watching for the stock directory entry that names the requested
symbol.

**This is a necessity, not an optimisation.** The execution, cancel, delete and
replace messages carry no symbol field at all. Only the locate. Any design that
filtered by comparing symbol strings would silently drop every modification to the
book and keep only the adds.

It is also much cheaper, since it compares two bytes rather than eight, on a path
that discards the large majority of messages in a real multi-symbol capture.

## Order replace loses queue priority

**Decision.** A `U` replace is implemented as a cancel of the original reference
followed by an add of the new one, at the back of its level's queue.

**Alternative considered.** Treat it as a modify, preserving queue position.

**Reasoning.** The message carries a *new order reference number* precisely because
the venue treats it as a new order. Implementing it as a modify would preserve a
queue position that the real book gave up, and the consequence is not abstract: it
would flatter every queue position estimate Phase 5 produces, and therefore every
P&L number that depends on one. `ItchReplay.ReplaceLosesQueuePriority` pins it.

The side is not in the replace message, so it has to be read from the order being
replaced before that order is removed. Getting that wrong would move liquidity to
the other side of the book, which is why there is a test for it specifically.

## Messages that do not touch the book

Non-cross trades (`P`), cross trades (`Q`), and broken trades (`B`) are counted and
ignored.

A `P` reports a trade against a *non-displayed* order. There is no resting order in
the visible book to adjust, so acting on it would remove liquidity that was never
there. Auction crosses execute outside the continuous book, and a broken trade is
an after-the-fact correction to a print rather than a change to resting liquidity.

## Memory mapping rather than reading

A full trading day of TotalView-ITCH is several gigabytes. Reading it into a buffer
costs that much resident memory plus a copy of every byte, and the copy is pure
waste because the parser is zero copy: it hands out spans into whatever memory the
bytes already occupy. Mapping lets the kernel page in what the parser touches and
drop it under pressure, so replay memory stays roughly constant regardless of file
size.

Both `mmap` and the Windows `CreateFileMapping` plus `MapViewOfFile` path are
implemented. That is not gold plating: the development host for this project is
Windows while the benchmark methodology targets Linux, and a parser that compiled
on only one of them would be untestable on the other.

## The synthetic generator, and why it must be semantically valid

**Decision.** `tools/itch_gen` writes real ITCH 5.0 binary, and the parser reads it
through exactly the same code path as a real capture with no branch anywhere on
where the bytes came from.

**Why it exists.** Real sample files are several gigabytes behind a NASDAQ
download. A repository whose tests only run for someone who already has one is a
repository whose tests nobody runs, CI included.

**Structural validity is not enough.** Executions, cancels, deletes and replaces
only ever name orders that are live at that point in the stream, and an execution
never exceeds an order's remaining size. A generator that emitted random order
references would produce a file the parser reads happily and the replay driver
rejects entirely, which would validate nothing. The generator therefore maintains
its own model of the live book, and that model doubles as the expectation the
replayed book is checked against, so validation does not depend on the parser
agreeing with itself.

The generated book is also never crossed, because a real venue's book cannot be.

## What a real NASDAQ capture confirmed, and what it revealed

The synthetic generator proves the parser is self-consistent and implements the
specification as written. It cannot prove the written specification and the actual
feed agree. So a real TotalView-ITCH capture was replayed: NASDAQ's public sample
for 2019-12-30, 3.5 GB compressed, of which the first 400 MB decompressed was
surveyed and replayed.

**What held.** Across 11 958 712 real messages the parser reported **zero unknown
message types**, so the length table covers the entire live feed. Framing was
exactly as assumed: a two byte big-endian length, then the message, with the symbol
space-padded at message offset 11. Replaying QQQ, 183 950 messages produced **zero
unknown order references**, meaning every execution, cancel, delete and replace
found the order it named, and the reconstructed book closed at 213.18 bid and
213.20 ask, which is QQQ's real price that day. A byte order error, a field offset
error, or a scale error does not produce numbers that happen to be right.

**What it revealed: real data contains prices a penny tick cannot express.** Five
of 92 705 QQQ adds were rejected as off-tick. NASDAQ quotes equities above a dollar
in pennies, but the price field is in hundredths of a cent and sub-penny prices do
occur. This is not a defect, it is the tick size being a property of the data rather
than of the code, and the trade-off is measurable:

| Tick size | Off-tick rejected | Rebases | Cold levels | Best bid / ask |
| --- | --- | --- | --- | --- |
| 100, a penny | 5 of 92 705 | 6 | 8 | 213.18 / 213.20 |
| 1, a hundredth of a cent | 0 | 1232 | 2101 | 213.18 / 213.20 |

A penny tick gives the band a 655 dollar span, so it rebases six times across a
session and holds almost nothing in cold storage, at the cost of discarding five
orders. A tick of one accepts every price, but the band spans 6.55 dollars and has
to chase the market instead of containing it: two hundred times the rebasing and
two hundred and sixty times the cold levels.

Both configurations agree on the top of book, which is what makes the penny tick
defensible: the orders it discards are never at the touch. Both are asserted in
`test_itch.cpp`, so a change in either number is noticed rather than absorbed.

**The counter was too coarse to say any of this.** `add_failures` lumped off-tick
prices together with band overflow and arena exhaustion. Those call for opposite
responses, one being a configuration choice and the other meaning liquidity was
silently lost, so it is now `off_tick_prices` and `rejected_adds`.

**And the synthetic mix was wrong.** Real data is 38.2 percent adds, 30.4 percent
deletes, and 1.0 percent executions. The generator's defaults produce roughly 11
percent executions, overstating them by an order of magnitude and understating
deletes. It also emits no net order imbalance messages at all, which are 9.0 percent
of the real feed. That matters for Phase 4: a throughput number measured on the
synthetic mix is not a throughput number for real data, and BENCHMARKS.md now says
so.

## Bugs caught by the tests, and what they teach

Recorded because a test suite that never caught anything is not evidence that the
code is correct, only that the tests are weak.

**1. Best price ignored a better cold level.** `best()` returned the band's best
whenever the band held anything, on the assumption that rebasing keeps every cold
level strictly worse. Two bids a million ticks apart falsified it: the far one was
the real best bid and the book reported the near one. Caught by
`BookBand.PricesOutsideTheBandReachTheColdPath`. The lesson is that an invariant
maintained by a heuristic is not an invariant. Rebasing is best effort, so nothing
downstream may treat its outcome as guaranteed.

**2. A half completed rebase misfiled every order in a stranded level.** When an
eviction to cold failed against a full cap, the code left the level in place and
carried on shifting the band, so the level's slot then mapped to a different price.
Found by tracing the first bug rather than by a test, then pinned by
`BookBand.RebaseIsAbandonedRatherThanLeftHalfDone`. The lesson is the fill-or-kill
lesson in a different costume: an operation that mutates many structures must
establish that it can finish before it starts.

**3. A benchmark measured DRAM and called it an add.** The first microbenchmark
reported 54 ns for an add into an existing level. The arena was sized at 2^20,
putting 64 MiB of working set against an 8 MiB L3, so the number was a memory
latency. Caught by sweeping the arena size instead of accepting the number. The
lesson is that an unexplained benchmark result is not a result.

**4. A benchmark's name did not match what it measured.** `bm_add_empty_level`
claimed to isolate the cost of the occupancy bitmap transition, and it came out
faster than a plain add, which is impossible if it does strictly more work. It
cancelled each order immediately, so the book stayed at one live order and
everything sat in L1 while the comparison benchmark grew to fill its arena. Renamed
to say what it does. The lesson is to check that a benchmark's result is possible
before quoting it.

**5. A test asserted a workload shape that the workload did not have.** A
differential case meant to exercise a deep sparse book placed orders uniformly
across a wide price span and asserted the book would end up with many occupied
levels. It ended up with two. Uniform placement across a wide span does not build
depth, it builds a repeatedly swept book, because a buy near the top of the span
crosses every ask beneath it. The generator gained a passive placement mode as a
result. The differential comparison itself never failed here, only the assertion
about what the test was testing, which is the more useful failure: a test that
silently exercises the wrong regime passes forever and proves nothing.

**6. The synthetic generator produced a completely static market.** The mid's
random walk truncated each step to a whole number of ticks, so any volatility below
one tick rounded every single step to zero and the price never moved. The default
volatility was 0.35 ticks, so the default configuration generated a market that did
not move at all. The mid is now carried as a fractional tick count and rounded only
when a price is written, and a deliberate drift parameter was added because a pure
random walk drifts as the square root of the message count and would need an
impractically long stream to reach a band edge by chance. Caught by a rebasing test
that reported zero rebases when it expected many. The lesson is that a test which
asserts a *consequence* of the workload, rather than the workload itself, catches
generator bugs that a structural check never would.

**7. A weight-based dispatch was wrong in a way that still produced a valid file.**
The generator's message mix selection used a lambda that decremented a running roll
and short-circuited over pairs of weights, then re-inspected the mutated roll to
choose within the pair. The resulting file parsed and replayed perfectly; the
message mix was simply not the mix that was configured. Found by reading it back
rather than by any test, which is the uncomfortable part: a wrong distribution is
invisible to every assertion that only checks validity. Replaced with explicit
cumulative selection.

**8. Fill-or-kill undercounted liquidity resting in cold storage.** The liquidity
precheck walked levels with `next_level_away`, which consulted only the band bitmap
and returned nothing the moment it left the band. The match loop calls `best()`
repeatedly, and `best()` does see cold levels, so the matcher would fill straight
through liquidity the precheck could not count. A fill-or-kill the matcher would have
filled in full was rejected as insufficient. Found by code review, not by the
test suite, and that is the interesting part: every differential configuration used a
price span of 2 to 900 ticks inside a 4096 level band, so no command sequence ever
combined fill-or-kill with cold levels. The three existing differential tests all pass
against the buggy code. `next_level_away` now walks the union of the band and cold
storage in price order, and a configuration whose price span exceeds the band was
added, which fails against the old code at command 154. The lesson is that a
randomised test only covers the regimes its generator can reach, and a blind spot in
the generator is invisible from inside the suite.

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
