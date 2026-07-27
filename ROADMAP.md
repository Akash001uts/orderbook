# Roadmap

Work that is agreed but not yet done, with enough context to pick it up cold.
Everything here is deliberately deferred rather than forgotten. Items are removed
from this file when they land, so anything still written down is still outstanding.

Progress and completed phases live in [README.md](README.md). Decisions and their
reasoning live in [DESIGN.md](DESIGN.md).

## Standing decisions

**Repository visibility.** Private until Phase 6 completes, then public. Not to be
changed before then.

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

- **Decide what to do about the best-price query.** No longer a measurement
  question. The Phase 4 baseline settled the size of it: the flat book's
  `best_bid()` costs 10.2 ns against the naive `std::map` book's 5.38 ns on an
  identical workload, so the bitmap descent loses to a tree that caches its extreme
  element as a pointer. Three dependent loads cannot beat one dereference. The
  options are to cache the best price per side and maintain it on the mutation
  paths, to return something cheaper than `std::optional`, or to accept the cost
  and say so. This needs a decision in the DESIGN.md decision/alternatives/cost
  format, not another benchmark.
- **HdrHistogram dependency.** Not fetched yet, deliberately. An unused dependency
  is still a dependency someone has to build. It arrives with the latency harness.
- **The measurement discipline itself**, which is the part of Phase 4 that is not
  a benchmark: core pinning through `sched_setaffinity`, configurable warmup, turbo
  and frequency-scaling detection, full environment capture, and run-to-run
  variance reported rather than a cherry-picked run. The microbenchmark set is
  complete without it, but no number from that set is quotable until this lands.

### Phase 6, documentation

- **Flip the repository to public** once documentation is complete.
- **Architecture diagram** in README, replacing the current ASCII sketch if a
  clearer form is warranted.
- **Audit for anything built but unexplained.** I want to do this deliberately
  at the end, and it is the check that catches decisions made silently during
  implementation.

## CI budget policy

The differential test's command budget is set by `OB_DIFF_COMMANDS` in
`.github/workflows/ci.yml`: one million on a push, ten million on the nightly
schedule or a manual dispatch.

The reason a reduced push budget is safe is that any failing sequence is shrunk to
a minimal reproducer and committed to `test/regressions/`. Once a bug has been
found at any budget, it is pinned by a deterministic test that runs on every push
regardless of this value. The long runs explore rarer state; they are not the thing
standing between a regression and `main`.
