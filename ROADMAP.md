# Roadmap

Work that is agreed but not yet done, with enough context to pick it up cold.
Everything here is deliberately deferred rather than forgotten. Items are removed
from this file when they land, so anything still written down is still outstanding.

Progress and completed phases live in [README.md](README.md). Decisions and their
reasoning live in [DESIGN.md](DESIGN.md).

## Standing decisions

**Repository visibility.** Private. Phase 6's documentation work is complete, but
the owner has asked that the repository stay private for now, so it does. Two
things gate any later flip: that instruction being lifted, and NASDAQ's terms on
the committed data slice being confirmed. Neither is a documentation task.

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
headline table and a five command reproduction path. What remains is a decision and
a permission, neither of which is a writing task.

- **Flipping the repository to public is deliberately deferred.** The owner asked
  that it not happen yet, so it does not. Nothing in the documentation blocks it.
- **Confirm NASDAQ's terms before any public flip.** This is the one genuine
  blocker rather than a preference. Committing a small derived slice for automated
  testing and republishing a complete copy of an exchange archive are different
  asks, even though NASDAQ hosts these publicly with no login. While the repository
  is private this is storage. If the answer turns out to be no, deleting
  `data/qqq_slice.itch` and relying on `scripts/fetch_nasdaq_sample.sh` costs only
  convenience, but the tests that assert against the committed slice would need
  their fixture regenerated on demand. Recorded also in `data/README.md`.

The architecture diagram was reviewed and kept as an ASCII sketch. It fits in a
terminal, it survives a plain text diff, and it already shows the strategy layer
feeding the same engine as the replay path, which is the one relationship a reader
has to understand. A rendered image would look better and say the same thing while
being invisible to every tool this project is read with.

## CI budget policy

The differential test's command budget is set by `OB_DIFF_COMMANDS` in
`.github/workflows/ci.yml`: one million on a push, ten million on the nightly
schedule or a manual dispatch.

The reason a reduced push budget is safe is that any failing sequence is shrunk to
a minimal reproducer and committed to `test/regressions/`. Once a bug has been
found at any budget, it is pinned by a deterministic test that runs on every push
regardless of this value. The long runs explore rarer state; they are not the thing
standing between a regression and `main`.
