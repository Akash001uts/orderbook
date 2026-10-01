# orderbook

[![ci](https://github.com/Akash001uts/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/Akash001uts/orderbook/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

A low latency limit order book and matching engine in C++20, with a NASDAQ
TotalView-ITCH 5.0 replay pipeline, a measurement-first benchmark harness, and a
market making strategy simulator that trades against the replayed book.

I built this as a student project to learn how exchanges match orders and what it
takes to make C++ fast *and* prove it is correct. I worked through it in phases,
each with something specific I wanted to understand, and wrote down what went wrong
along the way. [LEARNING.md](LEARNING.md) is the short version of that journey, and
the commit history tells it change by change.

The engine is deterministic by construction: it reads no clock, consumes no
randomness, performs no I/O, and allocates nothing after warmup. Every timestamp
arrives as a field on an input command. The same command sequence therefore
produces byte identical state on every run, which is what makes the differential
test against a naive reference implementation a meaningful correctness argument
rather than a smoke test.

## At a glance

| | |
| --- | --- |
| Real NASDAQ replay throughput | **37.8 M messages per second** |
| Per message latency, real capture | **p50 41 ns**, p99 158 ns, p99.9 227 ns |
| Against a `std::map` book | **2.8x** faster add, **5.3x** faster cancel, **2.1x** faster match |
| Allocations per add after warmup | **0**, against 2.064 for the `std::map` book |
| Correctness | Differential test against a reference book on every command, plus libFuzzer |
| CI | 16 jobs: GCC and Clang, ASan, UBSan, TSan, clang-tidy, clang-format, fuzzing |

Every number comes with the conditions that produced it in
[BENCHMARKS.md](BENCHMARKS.md), including the ones that are wrong. Everything above
reproduces on a fresh clone from the committed 5.5 MB NASDAQ slice; see
[Quick start](#quick-start).

## Features

**Matching engine** ([`include/ob/engine.hpp`](include/ob/engine.hpp))

- Limit, market, IOC, FOK, and post-only orders, plus cancel and modify with
  correct queue priority (a size reduction keeps its place, a size increase or price
  change goes to the back of the queue).
- Price-time priority matching across multiple levels in a single command.
- Self-trade prevention as a compile-time policy: cancel newest or cancel oldest.
- Events delivered to a caller-supplied sink, with a defined, fatal overflow policy
  rather than silent loss.
- Fully deterministic: no clock, no randomness, no I/O, no allocation after warmup.

**Order book** ([`include/ob/`](include/ob/))

- Flat price-level array, direct-indexed by tick offset, with a `std::map` overflow
  for prices outside the band kept off the hot path.
- Intrusive free-list order arena with 32-bit indices: `sizeof(Order)` is 40 bytes.
- Open-addressing order id map with linear probing and backward-shift deletion,
  for O(1) cancel.
- Hierarchical bitmap for O(1) best bid and ask, cached and repaired lazily.

**Market data** ([`itch/`](itch/))

- Zero-copy NASDAQ TotalView-ITCH 5.0 parser over a memory-mapped file (`mmap` and
  Windows file mapping), big-endian decode without `reinterpret_cast`, UBSan clean.
- Replay driver that turns ITCH messages into engine commands, and a synthetic ITCH
  generator for tests.
- Validated against a real NASDAQ capture from 2019-12-30: 8 906 symbols, zero
  unknown message types, and a reconstructed QQQ close that matches the real price.

**Strategy simulation** ([`strategy/`](strategy/))

- A market maker whose quotes enter the same engine as the replayed market, so it
  contends for real queue positions instead of being filled by an optimistic model.
- Queue position estimation, inventory skew, reaction-latency scenarios, P&L
  attribution, and markouts out to five minutes.

**Measurement and testing** ([`bench/`](bench/), [`test/`](test/))

- Google Benchmark microbenchmarks, an HdrHistogram latency harness on a pinned core
  with a verified TSC, and a paired `std::map` baseline in the same process.
- An exact allocation counter that replaces global `operator new`.
- A differential test that checks full book state after every randomised command,
  validated by injecting bugs, and a coverage-guided fuzz target sharing its code.

## Quick start

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

### Run the results site locally

The project includes a small static results site (Vite, TypeScript, uPlot) that
charts the replay, latency, and backtest results per symbol. It is not hosted yet,
so for now run it on your own machine. It needs Node 22 and no C++ toolchain, and it
reads the JSON artifacts committed under `site/data/`, so it works on a fresh clone:

```bash
cd site
npm install
npm run dev        # serves on http://localhost:5180
```

For a production build instead, `npm run build` writes a static site to `site/dist/`
and `npm run preview` serves it. [site/README.md](site/README.md) covers where the
data comes from and how to regenerate it.

The headline `std::map` comparison shown on the site is read from a single
canonical artifact, `site/data/bench/baseline.json`, which is also the source the
baseline and allocation tables in [BENCHMARKS.md](BENCHMARKS.md) are rendered from
by `scripts/sync_benchmarks.py`. A CI job runs that script in `--check` mode, so the
site, the document, and the artifact cannot disagree on those two comparison tables.
Other numbers in the prose, such as the arena curve, are not covered by this guard.

## Results

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
element while a bitmap descent is three dependent loads. I reported that rather
than burying it, and then fixed it by caching the final answer and repairing it lazily.
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

### Strategy

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

### Using the engine

A runnable example, [`examples/engine_usage.cpp`](examples/engine_usage.cpp),
constructs an engine, submits limit and marketable orders, drains the events, and
shows that an invalid configuration is rejected at construction. It builds with the
default options and runs on a fresh clone:

```bash
cmake --preset release
cmake --build --preset release --target ob_example
./out/build/release/ob_example
```

It is also registered as a CTest smoke test, so `ctest --preset release -R Example`
builds and runs it as part of the suite.

The engine delivers events to a **sink**, any type exposing
`bool push(const ExecutionEvent&)`. It returns `false` only when the sink is full
and `true` otherwise; an unbounded or discarding sink returns `true` unconditionally.
`EventRing<Capacity>` is the ready-made bounded sink.

- **Overflow is fatal, not recoverable.** `submit` is `noexcept` and does not roll
  back. If a `push` returns `false`, the engine aborts the process in debug and
  release alike rather than dropping an event, continuing, or unwinding a
  half-applied command. Losing an event would desynchronise any consumer
  reconstructing state from the stream.
- **Size the sink for the largest burst a single command can emit**, not one event
  per level. A marketable order emits a fill per resting order it consumes across
  every level it sweeps, plus a terminal event; self-trade prevention can emit a
  cancel per resting order removed; a modify or resting add emits a book update.
  Drain the sink between commands, or size it for the accumulated total if you do
  not.
- **Source-compatibility note.** `push` returns `bool`. It previously returned
  `void`, so a custom sink declaring `void push(const ExecutionEvent&)` no longer
  compiles and must be updated to return `bool`. The engine reads the result to
  enforce the overflow policy above.

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
| `site/` | The static results site and the JSON artifacts it reads, see `site/README.md` |

## Documents

| | |
| --- | --- |
| [DESIGN.md](DESIGN.md) | Every non-obvious decision, the alternatives considered, the reasoning, and the places this design is the wrong choice. The single source of truth. |
| [BENCHMARKS.md](BENCHMARKS.md) | Measured results with the conditions that produced them, and what is wrong with them. |
| [STRATEGY.md](STRATEGY.md) | The market maker, queue position estimation, P&L attribution, and the markout reading. |
| [LEARNING.md](LEARNING.md) | What I set out to learn in each phase, what went wrong, and what I took from it. |
| [ROADMAP.md](ROADMAP.md) | Deferred work, and what is deliberately kept cheap. |
| [site/README.md](site/README.md) | The results site: how to run it, where its data comes from, and how to deploy it. |
| [data/README.md](data/README.md) | Market data provenance, and why the full capture is a release asset rather than a repository file. |

## Scope

Deliberately not built: network transport, FIX or SBE gateways, multi-symbol
thread sharding, persistence or a write-ahead log, clustering, a live order-entry or
trading UI, and any live trading connection. There is a web results site (see
[site/README.md](site/README.md)); what is out of scope is an interactive
order-entry front end, not a web presence. The reasoning for each exclusion is
recorded in [DESIGN.md](DESIGN.md).

## Project status

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
changes what a number means. Every number, with the conditions that produced it, is
in [BENCHMARKS.md](BENCHMARKS.md).

A local `ctest --preset debug` registers 162 tests, 161 pass, and one is skipped:
the differential regression replay stays skipped until a shrunk fixture is
committed. Run the command yourself to confirm the count rather than trusting this
line.

## Licence

The original source code and documentation are released under the MIT license,
see [LICENSE](LICENSE).

That grant does not extend to the Nasdaq-sourced market data: the committed slice
at `data/qqq_slice.itch` and the derived market-data artifacts under `site/data/`
are excluded from the MIT license and remain subject to Nasdaq's applicable terms.
The exclusion is stated in [NOTICE](NOTICE), and the provenance and required
attribution are in [data/README.md](data/README.md).
