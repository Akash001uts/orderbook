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

Phase 0 of 6 complete. The type layer, event model, build system, and CI are in
place and green on GCC and Clang.

| Phase | Scope | State |
| --- | --- | --- |
| 0 | Scaffold, strong types, event model, presets, CI | complete |
| 1 | Flat direct-indexed book, order arena, open-addressing id map, bitmap best price | not started |
| 2 | Matching engine, order types, differential test against a `std::map` oracle | not started |
| 3 | ITCH 5.0 zero-copy parser, replay driver, synthetic file generator | not started |
| 4 | Microbenchmarks, HdrHistogram latency harness, `std::map` baseline comparison | not started |
| 5 | Market maker, queue position estimator, P&L attribution, markouts | not started |
| 6 | Documentation pass | not started |

Benchmark numbers are published in [BENCHMARKS.md](BENCHMARKS.md) once Phase 4
lands. Nothing is claimed here that has not been measured.

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
