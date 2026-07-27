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

Phase 3 of 6 complete and validated against a real NASDAQ capture. The book, the
matching engine, and the ITCH 5.0 pipeline are built and tested: 93 tests green on
GCC and Clang across debug, release, relwithdebinfo, and all three sanitizers, with
14 CI jobs covering both compilers, every preset, all three sanitizers, clang-tidy,
clang-format, the differential test, and a libFuzzer run.

Phase 4 needs.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Scaffold, strong types, event model, presets, CI | complete |
| 1 | Flat direct-indexed book, order arena, open-addressing id map, bitmap best price | complete |
| 2 | Matching engine, order types, differential test against a `std::map` oracle | complete |
| 3 | ITCH 5.0 zero-copy parser, replay driver, synthetic file generator | complete |
| 3b | Validated against a real NASDAQ TotalView capture | complete |
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

### Validated against real market data

Synthetic tests prove the parser is self-consistent. They cannot prove the
specification and the live feed agree, so a real NASDAQ TotalView-ITCH capture was
replayed: 2019-12-30, 11 958 712 messages, 8 906 symbols.

| | |
| --- | --- |
| Unknown message types | 0, so the length table covers the whole live feed |
| Unknown order references, QQQ | 0 of 183 950 messages |
| Reconstructed QQQ close | 213.18 bid / 213.20 ask, the real price that day |

It also found something: five of 92 705 QQQ adds are priced off a penny boundary,
because ITCH prices are in hundredths of a cent and sub-penny prices occur. The
trade-off between tick size and band churn is measured in
[DESIGN.md](DESIGN.md).

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
| ITCH parse only | 312 M msg/s, 9.7 GiB/s |
| ITCH parse and replay | 15.7 M msg/s |

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

No market data download is needed. A 5.5 MB slice of a real NASDAQ capture is
committed in `data/`, and `itch_gen` writes synthetic ITCH 5.0 binary that the parser
reads through exactly the same code path:

```bash
./out/build/release/itch_gen --messages 1000000 --symbol AAPL data/sample.itch
./out/build/release/itch_replay --symbol QQQ data/qqq_slice.itch
```

For a full trading day, `scripts/fetch_nasdaq_sample.sh` downloads one from NASDAQ's
public archive. They are 3.5 to 4.8 GB compressed, which is why only the slice is
committed. `itch_replay --survey` reports the message histogram and busiest symbols
of whatever you point it at.

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
| `tools/` | Synthetic ITCH generator and the replay CLI |
| `data/` | A committed slice of a real NASDAQ capture, see `data/README.md` |
| `scripts/` | Fetch a NASDAQ sample day, publish a full capture as release assets |

## Documents

| | |
| --- | --- |
| [DESIGN.md](DESIGN.md) | Every non-obvious decision, the alternatives considered, the reasoning, and the places this design is the wrong choice. The single source of truth. |
| [BENCHMARKS.md](BENCHMARKS.md) | Measured results with the conditions that produced them, and what is wrong with them. |
| [ROADMAP.md](ROADMAP.md) | Agreed but deferred work, and what is deliberately kept cheap. |
| [data/README.md](data/README.md) | Market data provenance, and why the full capture is a release asset rather than a repository file. |

## Scope

Deliberately not built: network transport, FIX or SBE gateways, multi-symbol
thread sharding, persistence or a write-ahead log, clustering, a web UI, and any
live trading connection. The reasoning for each exclusion is recorded in
[DESIGN.md](DESIGN.md).
