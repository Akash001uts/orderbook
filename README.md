# orderbook

A low latency limit order book and matching engine in C++20, with a NASDAQ
TotalView-ITCH 5.0 replay pipeline, a measurement-first benchmark harness, and a
market making strategy simulator that trades against the replayed book.

The engine is deterministic by construction: it reads no clock, consumes no
randomness, performs no I/O, and allocates nothing after warmup. Every timestamp
arrives as a field on an input command. The same command sequence therefore
produces byte identical state on every run, which is what makes the differential
test against a naive reference implementation a meaningful correctness argument
rather than a smoke test.

## Status

Phase 2 of 6 complete. The book and the matching engine are built and tested,
green on GCC and Clang across debug, release, and all three sanitizers.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Scaffold, strong types, event model, presets, CI | complete |
| 1 | Flat direct-indexed book, order arena, open-addressing id map, bitmap best price | complete |
| 2 | Matching engine, order types, differential test against a `std::map` oracle | complete |
| 3 | ITCH 5.0 zero-copy parser, replay driver, synthetic file generator | not started |
| 4 | Microbenchmarks, HdrHistogram latency harness, `std::map` baseline comparison | not started |
| 5 | Market maker, queue position estimator, P&L attribution, markouts | not started |
| 6 | Documentation pass | not started |

A first look at the numbers is in [BENCHMARKS.md](BENCHMARKS.md), published with
the reasons it is not yet trustworthy. The disciplined harness lands in Phase 4.
Nothing is claimed here that has not been measured.

### Correctness

Every randomised command is fed to both the real engine and a deliberately naive
`std::map` reference book, and full state equivalence is asserted after every
single command: best bid, best ask, per level aggregate quantity, per level order
count, the exact ordered sequence of order ids at every occupied level, and the
emitted event stream. A mismatch shrinks itself to a minimal reproducer, which is
committed and replayed on every push from then on.

The test itself has been validated by mutation rather than assumed to work. Three
deliberate bugs were injected and each was caught by a different one of its checks:

| Injected bug | Caught by | At command |
| --- | --- | --- |
| Crossing predicate `<=` changed to `<` | event stream | 3 |
| Partial fill skips the level aggregate | aggregate quantity | 9 |
| Quantity reduction requeues instead of holding its place | queue order | 399 |

One million commands run on every push, ten million nightly, plus a coverage
guided libFuzzer target sharing the same comparison code.

### Measured so far

| | |
| --- | --- |
| `sizeof(Order)` | 40 bytes, 32-bit arena indices rather than pointers |
| `sizeof(PriceLevel)` | 24 bytes, 2.67 levels per cache line |
| Footprint at defaults | 19.3 MiB: 3 MiB band, 16 KiB bitmaps, 10 MiB arena, 6 MiB id map |
| Id map load factor | 0.50, mean probe count 1.0 |
| Order id lookup | 2.3 ns |
| Cancel from level head | 18.7 ns |
| Add to an existing level | 22.8 ns |

Measured on Windows with no core pinning on a hybrid CPU, so treat differences
under a factor of two as noise. The conditions are spelled out in full in
[BENCHMARKS.md](BENCHMARKS.md).

## Build and run

Requires CMake 3.24 or newer, Ninja, and either GCC 13 or newer or Clang 17 or
newer. Dependencies are fetched by CMake, so a clone and a configure is enough.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `relwithdebinfo`, `asan`, `tsan`, `ubsan`. The three
sanitizer presets require a Linux or macOS host; the sanitizer runtimes are not
available on Windows targets.

To build with a specific compiler, set `CC` and `CXX` before configuring:

```bash
CC=clang CXX=clang++ cmake --preset debug
```

## Architecture

```
                    ITCH 5.0 file (mmap, zero copy)
                                 |
                    itch/parser  |  big-endian decode, no reinterpret_cast
                                 v
                    itch/replay  |  message semantics to engine commands
                                 v
   strategy/ ---> quote cmds ---> ob::Engine ---> EventRing<ExecutionEvent>
   market_maker <--- fills ------      |                    |
   queue est.                          |                    v
   pnl + markouts                      v            bench/latency_harness
                                   ob::Book              HdrHistogram
                                       |
        +------------------------------+------------------------------+
        |                    |                     |                 |
   PriceLevel[]         Order arena          OrderId map        level bitmap
   flat, direct         intrusive free    open addressing,   hierarchical,
   indexed by tick      list, 32-bit      linear probing,    countr_zero for
   offset from band     arena indices     backward shift     best bid and ask
   base                                   deletion
```

The strategy's own orders enter the same engine as the replayed market's orders,
so they contend for real queue positions rather than being filled by a separate
optimistic model.

## Layout

| Path | Contents |
| --- | --- |
| `include/ob/` | Core book and engine, header only where it must inline |
| `src/` | Book and engine translation units |
| `itch/` | ITCH 5.0 parser and replay driver |
| `strategy/` | Market maker, inventory, P&L |
| `bench/` | Google Benchmark microbenchmarks and the latency harness |
| `test/` | Unit, differential, and fuzz tests, plus the reference oracle |
| `tools/` | Synthetic ITCH file generator |

## Design

Every non-obvious decision, the alternatives considered, and the reasoning are in
[DESIGN.md](DESIGN.md), including the places this design is the wrong choice.

## Scope

Deliberately not built: network transport, FIX or SBE gateways, multi-symbol
thread sharding, persistence or a write-ahead log, clustering, a web UI, and any
live trading connection. The reasoning for each exclusion is recorded in
[DESIGN.md](DESIGN.md).
