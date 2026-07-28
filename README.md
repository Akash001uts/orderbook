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

All six phases complete. The book, the matching engine, the ITCH 5.0 pipeline, the
benchmark harness, the market making strategy layer, and the documentation are
built and tested: 127 tests green on GCC and Clang across debug, release,
relwithdebinfo, and all three sanitizers, with 14 CI jobs covering both compilers,
every preset, all three sanitizers, clang-tidy, clang-format, the differential
test, and a libFuzzer run. The repository is private.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Scaffold, strong types, event model, presets, CI | complete |
| 1 | Flat direct-indexed book, order arena, open-addressing id map, bitmap best price | complete |
| 2 | Matching engine, order types, differential test against a `std::map` oracle | complete |
| 3 | ITCH 5.0 zero-copy parser, replay driver, synthetic file generator | complete |
| 3b | Validated against a real NASDAQ TotalView capture | complete |
| 4 | Microbenchmarks, HdrHistogram latency harness, `std::map` baseline comparison | complete |
| 5 | Market maker, queue position estimator, P&L attribution, markouts | complete |
| 6 | Documentation pass | complete |

Nothing is claimed here that has not been measured, and the one condition that
could not be met on any available host, an isolated core, is named wherever it
changes what a number means. The headline figures are below; every number with the
conditions that produced it is in [BENCHMARKS.md](BENCHMARKS.md).

The strategy results are in [STRATEGY.md](STRATEGY.md), and the fill model is
stated before any P&L because every number is downstream of it. The baseline market
maker loses money, and the attribution says why: with inventory skew disabled it had
no way to get flat and carried a thousand shares through a dollar decline. Turning
skew on cuts the inventory loss by a factor of 3.5 while leaving spread capture
unchanged.

That document also records a conclusion it had to withdraw. It previously reported
that the fills themselves were fine, which was true out to ten seconds and false at
five minutes, where the aggregate markout turns negative. The horizon set decided
what the metric could detect, and stopping early certified something that was not
happening.

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
| Add | 30.5 ns | 84.7 ns | 2.8x faster |
| Cancel | 11.5 ns | 61.1 ns | 5.3x faster |
| Match, one level consumed | 32.2 ns | 67.4 ns | 2.1x faster |
| Best bid query | **0.26 ns** | 4.45 ns | below the harness noise floor |

The last row has a history worth knowing. The flat book originally **lost** this
query, 10.2 ns against the tree's 5.38 ns, because `std::map` caches its extreme
element while a bitmap descent is three dependent loads. That was reported rather
than buried, and then fixed by caching the final answer and repairing it lazily.
The 0.26 ns is only about 0.10 ns above an empty loop, so the accurate claim is
that the query is below what the harness can resolve rather than that it costs any
particular number; a dedicated noise-floor benchmark exists to make that
distinction checkable.

The mechanism behind the ratio is counted rather than inferred, because no
available host exposes hardware performance counters: the flat book allocates
**0** times per add, the `std::map` book **2.064**, a figure that decomposes
exactly into a list node, a hash node, and one map node per new level.

Both arms run at a 16 384 slot arena, matched to the roughly 8 192 live orders the
benchmark holds. That is deliberately **not** the library default of 65 536:
measuring 8 192 orders in a 2.5 MiB arena would report a DRAM latency rather than a
property of the book.

The default is larger because peak book depth varies 62-fold across mainstream
symbols on one ordinary day, from IWM at 434 live orders to AAPL at 27 097, so it is
sized to the deepest measured name. A book known to be shallow should lower it and
take the speed. Both numbers, and what the difference costs, are in
[BENCHMARKS.md](BENCHMARKS.md).

Absolute figures come from paired runs, because this machine drifted by up to 2.6x
across a session on an unchanged binary. The ratios survive that; the nanoseconds
are one afternoon's reading.

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
| Footprint at defaults | about 7.2 MiB: 3 MiB band, 16 KiB bitmaps, 2.5 MiB arena, 1.5 MiB id map |
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
