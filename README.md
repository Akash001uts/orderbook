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

Phase 5 of 6 complete. The book, the matching engine, the ITCH 5.0 pipeline, the
benchmark harness, and the market making strategy layer are built and tested: 116
tests green on GCC and Clang across debug, release, relwithdebinfo, and all three
sanitizers, with 14 CI jobs covering both compilers, every preset, all three
sanitizers, clang-tidy, clang-format, the differential test, and a libFuzzer run.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Scaffold, strong types, event model, presets, CI | complete |
| 1 | Flat direct-indexed book, order arena, open-addressing id map, bitmap best price | complete |
| 2 | Matching engine, order types, differential test against a `std::map` oracle | complete |
| 3 | ITCH 5.0 zero-copy parser, replay driver, synthetic file generator | complete |
| 3b | Validated against a real NASDAQ TotalView capture | complete |
| 4 | Microbenchmarks, HdrHistogram latency harness, `std::map` baseline comparison | complete |
| 5 | Market maker, queue position estimator, P&L attribution, markouts | complete |
| 6 | Documentation pass | not started |

Nothing is claimed here that has not been measured, and the one condition that
could not be met on any available host, an isolated core, is named wherever it
changes what a number means. The headline figures are below; every number with the
conditions that produced it is in [BENCHMARKS.md](BENCHMARKS.md).

The strategy results are in [STRATEGY.md](STRATEGY.md), and the fill model is
stated before any P&L because every number is downstream of it. The baseline market
maker loses money, and the attribution says why: its fills were good at every
markout horizon out to ten seconds, but with inventory skew disabled it had no way
to get flat and carried a thousand shares through a dollar decline. Turning skew on
cuts the inventory loss by a factor of 3.5 while leaving spread capture unchanged.

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

### Headline numbers

**Against a naive `std::map` book on an identical workload**, both running in one
process under the same conditions, so the ratio is the durable part:

| Operation | Flat book | `std::map` book | |
| --- | --- | --- | --- |
| Add | 26.9 ns | 73.5 ns | 2.7x faster |
| Cancel | 10.3 ns | 56.0 ns | 5.4x faster |
| Match, one level consumed | 38.7 ns | 65.2 ns | 1.7x faster |
| Best bid query | 10.2 ns | 5.38 ns | **1.9x slower** |

The last row is the one worth reading. A tree caches its extreme element as a
pointer, and the bitmap's three dependent loads cannot beat one dereference. It is
reported because a comparison that only went one way would be advertising.

The mechanism behind the ratio is counted rather than inferred, because no
available host exposes hardware performance counters: the flat book allocates
**0** times per add, the `std::map` book **2.016**, a figure that decomposes
exactly into a list node, a hash node, and one map node per new level.

**Replaying a real NASDAQ capture**, per message end to end, on a pinned core with
an invariance-verified TSC:

| | p50 | p90 | p99 | p99.9 | p99.99 |
| --- | --- | --- | --- | --- | --- |
| All messages | 41 ns | 79 ns | 158 ns | 227 ns | 549 ns |

37.8 M messages per second, measured on separate uninstrumented passes because two
timestamps per message cost enough to change the answer by a factor of 3.5.

**Structure**, which does not vary between runs:

| | |
| --- | --- |
| `sizeof(Order)` | 40 bytes, 32-bit arena indices rather than pointers |
| `sizeof(PriceLevel)` | 24 bytes, 2.67 levels per cache line |
| Footprint at defaults | 19.3 MiB: 3 MiB band, 16 KiB bitmaps, 10 MiB arena, 6 MiB id map |
| Id map | load factor 0.50, mean probe count 1.0, lookup 2.3 ns |

The same replay was run under GCC 16.1.0 on Windows and GCC 13.3.0 on Linux. Two
independently calibrated TSC frequencies agree to five digits and the percentiles
agree within a few nanoseconds, which is the evidence that the measurement is sound
rather than an artifact of one toolchain.

Every condition, including the ones that are wrong, is in
[BENCHMARKS.md](BENCHMARKS.md). The short version: the harness pins, warms, and
verifies its clock, but no available host offers an isolated core, so percentiles
up to p99.99 are sound and maximum values are not.

## Build and run

Requires CMake 3.24 or newer, Ninja, and either GCC 13 or newer or Clang 17 or
newer. Dependencies are fetched by CMake, so a clone and a configure is enough.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

**No market data download is needed.** A 5.5 MB slice of a real NASDAQ capture is
committed in `data/`, so every command below runs on a fresh clone:

```bash
./out/build/release/itch_replay --symbol QQQ data/qqq_slice.itch
./out/build/release/ob_baseline_bench --benchmark_repetitions=9 --benchmark_report_aggregates_only=true
./out/build/release/ob_latency_bench --file data/qqq_slice.itch --symbol QQQ --cpu 2
./out/build/release/ob_strategy_backtest --file data/qqq_slice.itch --symbol QQQ --skew 2
```

Those four reproduce, in order, the real-capture replay, the `std::map` comparison,
the per message latency distribution, and the market making backtest. `itch_gen`
also writes synthetic ITCH 5.0 binary that the parser reads through exactly the same
code path, which is what the differential and fuzz tests consume.

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
