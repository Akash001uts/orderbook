# Roadmap

Work that is agreed but not yet done, with enough context to pick it up cold.
Everything here is deliberately deferred rather than forgotten. Items are removed
from this file when they land, so anything still written down is still outstanding.

Progress and completed phases live in [README.md](README.md). Decisions and their
reasoning live in [DESIGN.md](DESIGN.md).

## Standing decisions

**Repository visibility.** Prepared for public release. Phase 6's documentation work
is complete and the repository is licensed under MIT, with the Nasdaq-sourced data
excluded from that grant. Flipping the repository to public is the owner's remaining
step and is deliberately left until this work is merged and CI is green. See LICENSE,
NOTICE, and data/README.md.

**Effort allocation.** The four items below taught me the most relative to their
cost, so they got disproportionate effort. Everything else is built
to do its job without gold plating.

| Item | Phase | Why it earns the effort |
| --- | --- | --- |
| Differential test against a naive oracle | 2 | The strongest single artifact here. A fast data structure is common; proof that a fast data structure is correct is not. Cheap to build relative to what it demonstrates. |
| `std::map` baseline comparison | 4 | Nearly free, because the reference book already exists for the differential test. The speedup ratio with a mechanical explanation is the most persuasive number in the repository. |
| Flat direct-indexed book | 1 | Complete. This is the core data structure the whole project is built around. |
| README and DESIGN | 6 | Determines whether any of the above is ever read. |

**Deliberately kept cheap.** These are in scope and will be built, but they are
not where the effort goes, and the reason is recorded so that a later reader does
not mistake brevity for an oversight.

- **libFuzzer target.** The seeded differential test already explores the same
  command space with a stronger oracle. The fuzz target reuses that command
  encoding and stops there.
- **Parameter sweep.** A CSV and a minimal plot, framed explicitly as sensitivity
  analysis. Presenting the best cell as a discovered edge would be worse than
  omitting the sweep entirely.
- **Self-trade prevention policies.** One policy, cancel-newest, behind a template
  parameter. The signal is that the policy is swappable. Writing three
  implementations of it adds nothing.

## Host prerequisites

**WSL2, needed before Phase 4. Satisfied on 2026-07-27.** Ubuntu-24.04 is
installed and boots. No reboot was required: the WSL platform and kernel were
already present and only the distribution was missing. Core pinning is verified
working inside the guest, so the `sched_setaffinity` path Phase 4 builds has a host
that can exercise it.

Phase 3 turned out not to need it after all: `MappedFile` implements both the
`mmap` and the Windows `CreateFileMapping` paths, so the ITCH pipeline is fully
testable on either host. Phase 4 is where it matters, for `sched_setaffinity` and
core isolation, neither of which has a Windows equivalent.

**Hardware performance counters, still not satisfied and not closable here.**
`perf stat` cache-miss counters require bare-metal Linux. This was checked inside
the new guest rather than assumed: `/sys/bus/event_source/devices/` lists only
`breakpoint`, `kprobe`, `msr`, `power`, `software`, `tracepoint`, and `uprobe`,
with no `cpu` source of any kind, so there is no hardware counter to read.
GitHub-hosted runners do not expose one either, so neither host closes this gap.

The mitigation is already built and working: `bench/micro_bench.cpp` sweeps the
arena capacity with the algorithmic work held constant, so the entire shape of the
resulting curve is cache behaviour. The Phase 1 curve breaks exactly at this
machine's L2 size. That isolates cache effects by construction rather than by
reading a counter, and it works on any host. Phase 4 extends the same technique
across the other operations.

`perf stat` counters remain the nicer artifact if a bare-metal Linux machine ever
becomes available. They are not a blocker.

## Deferred work by phase

### Phase 2, matching engine

Complete. One item deliberately not built:

- **Further self trade prevention policies.** `CancelNewest` and `CancelOldest`
  are implemented and both are covered by the differential test. Cancel-both and
  decrement-and-cancel are described in DESIGN.md with the reasoning for leaving
  them out: neither demonstrates a mechanism the existing pair does not.

### Phase 3, ITCH 5.0 pipeline

Complete, including validation against a real NASDAQ TotalView capture. Nothing
deferred.

One new item for Phase 6, from what the real capture revealed: the synthetic
generator's message mix differs materially from the real feed, overstating executions
by roughly ten times and emitting no imbalance messages. Bringing the defaults closer
to observed proportions would make every synthetic benchmark more representative. It
is not urgent, because Phase 4 measures the real capture separately rather than
relying on the synthetic figure, but it is worth doing before anyone quotes a
synthetic number as a proxy for real throughput.

### Phase 4, benchmark harness

- **The best-price query is done.** It was the one operation the flat book lost,
  10.2 ns against the naive book's 5.38 ns. Resolved by caching the final answer to
  `best()` and repairing it lazily when the best level empties, which puts the query
  below the harness's noise floor while leaving add and cancel unchanged within
  noise. Results in BENCHMARKS.md, reasoning in `book.hpp`, and six dedicated tests
  plus the differential suite guard the cache.

Phase 4 is otherwise complete: the microbenchmark set, the `std::map` baseline,
the HdrHistogram latency harness, and the measurement discipline all landed.

- **An isolated core, which no host here can provide.** Pinning is implemented and
  verified on both platforms, but pinning is not isolation. Windows has no
  `isolcpus` equivalent and WSL2 is a guest, so the OS can still schedule other
  work onto the pinned core. The consequence is bounded and stated wherever it
  matters: percentiles up to p99.99 are sound, maximum values are not. Closing
  this needs bare-metal Linux booted with `isolcpus` and `nohz_full`, which is the
  same machine that would close the `perf stat` gap above.

### Phase 6, documentation

The documentation work itself is done: the duplication the code review flagged
is resolved, the design document has been audited against the code, and the README carries the
headline table and a five command reproduction path.

- **The repository is licensed under MIT and ready to go public.** Nothing in the
  documentation blocks the flip; making it is the owner's remaining step, deferred
  until this work is merged and CI is green.
- **The market data handling is defined.** The Nasdaq-sourced slice and the derived
  artifacts are excluded from the MIT grant and redistributed on the assumption that
  Nasdaq's permission is granted, subject to Nasdaq's terms, with the attribution
  recorded in [data/README.md](data/README.md). Obtaining and retaining that
  permission record is handled separately from this documentation.
  `scripts/publish_dataset.sh` is still run deliberately rather than automatically,
  because publishing the complete archive is the larger ask of the two.

The architecture diagram was reviewed and kept as an ASCII sketch. It fits in a
terminal, it survives a plain text diff, and it already shows the strategy layer
feeding the same engine as the replay path, which is the one relationship a reader
has to understand. A rendered image would look better and say the same thing while
being invisible to every tool this project is read with.

## Next work: the frontend

Accepted from the third review, not started. The sequencing matters more than the
choice of framework.

**1. Machine-readable output first.** `itch_replay`, `ob_strategy_backtest` and
`ob_latency_bench` all print human-formatted text, and this repository changes its
wording deliberately and often. A frontend that scrapes stdout would break on the
next edit. Each tool needs a `--json` flag emitting a stable schema; the structs
already exist, `ReplayStats`, `PnlAccount`'s accessors, `CrossedBookStats`, and the
sweep rows.

**2. Static charts second.** Everything this project produces is a static result of
a deterministic replay: benchmark tables, sweep grids, markout curves, crossed-book
statistics. A static site rendering committed JSON covers essentially the full
value, needs no C++ toolchain to view, and sidesteps every question about running
native code behind a web server.

**3. WebAssembly only if an interactive demo earns its cost.** A live "submit an
order, watch the book" demo needs the engine compiled to WASM or a backend service,
which is a different and much larger project. The band, arena and event ring would
compile without change, since there are no threads and no syscalls on the hot path,
so the door stays open rather than being taken now.

## CI budget policy

The differential test's command budget is set by `OB_DIFF_COMMANDS` in
`.github/workflows/ci.yml`: one million on a push, ten million on the nightly
schedule or a manual dispatch.

The reason a reduced push budget is safe is that any failing sequence is shrunk to
a minimal reproducer and committed to `test/regressions/`. Once a bug has been
found at any budget, it is pinned by a deterministic test that runs on every push
regardless of this value. The long runs explore rarer state; they are not the thing
standing between a regression and `main`.
