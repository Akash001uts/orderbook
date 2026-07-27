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

## Prerequisites not yet satisfied

**WSL2, needed before Phase 4.** Installation was started and needs a reboot to
finish:

```
wsl --install -d Ubuntu-24.04
```

Phase 3 turned out not to need it after all: `MappedFile` implements both the
`mmap` and the Windows `CreateFileMapping` paths, so the ITCH pipeline is fully
testable on either host. Phase 4 is where it matters, for `sched_setaffinity` and
core isolation, neither of which has a Windows equivalent.

**Hardware performance counters, needed for the full Phase 4 method.** `perf stat`
cache-miss counters require bare-metal Linux. WSL2 runs under a hypervisor that
does not expose the PMU to the guest, and GitHub-hosted runners do not expose it
either, so neither of those closes this gap.

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

- **`std::map` baseline comparison.** Blocked on `test/reference_book.hpp`, which
  Phase 2 creates, and on the latency harness. Publish both implementations'
  numbers on an identical workload, with the speedup ratio explained rather than
  just stated. What does the explaining is settled and written down in
  BENCHMARKS.md: allocation counts from the existing `operator new` replacement, a
  working set sweep run on both implementations, and a structural pointer hop count
  per operation. Not `perf stat`, which no host here can read.
- **Resolve the best-price query cost.** Phase 1 measured 13.4 ns for
  `best_bid()`, which is slower than three dependent loads into 8 KiB of resident
  structure plus three bit instructions should cost. Candidates are the
  `std::optional` return being materialised to memory rather than kept in
  registers, and the benchmark's `DoNotOptimize` barrier forcing a store. Recorded
  in BENCHMARKS.md as open.
- **Isolate the occupancy bitmap transition properly.** The Phase 1 attempt
  compared an add-into-occupied-level against an add-then-cancel that kept the book
  at one live order, so the two had entirely different working sets and the
  comparison was meaningless. A correct version holds the working set fixed across
  both arms.
- **HdrHistogram dependency.** Not fetched yet, deliberately. An unused dependency
  is still a dependency someone has to build. It arrives with the latency harness.

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
