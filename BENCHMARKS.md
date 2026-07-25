# Benchmarks

**No numbers are published in this document yet.** The measurement harness lands
in Phase 4. What follows is the methodology it will implement, recorded before any
measurement is taken so that the method cannot be chosen retroactively to suit a
result.

An unmeasured performance claim is worthless, and a measured claim without its
methodology is not much better. Both the numbers and the conditions that produced
them are deliverables here.

## What will be measured

**Microbenchmarks**, via Google Benchmark, one case each for:

| Case | Why it is separated out |
| --- | --- |
| add to an empty level | Exercises the level occupancy transition and the bitmap set |
| add to an existing level | The common case, no bitmap work, tail append only |
| cancel from the list head | Best case unlink, and the case that empties a level |
| cancel from the list middle | The case a pointer-chasing design loses on |
| cancel from the list tail | Tests the tail index maintenance path |
| modify down | In-place quantity mutation, queue position preserved |
| aggressive order crossing one level | Single level consumption |
| aggressive order crossing ten levels | Level walking and repeated bitmap clears |
| best price query | The bitmap `countr_zero` path in isolation |

**End-to-end replay latency**: per message latency across a full ITCH replay,
recorded into an HdrHistogram and reported as p50, p90, p99, p99.9, p99.99, and
max in nanoseconds, broken out by message type, plus throughput in messages per
second. Averages will not be reported alone. A mean hides the tail, and the tail
is the number that matters in this domain.

**Comparative baseline**: the naive `std::map<Price, std::list<Order>>` reference
book from `test/reference_book.hpp` measured on the identical workload, with both
numbers published. The speedup ratio is only meaningful alongside a mechanical
explanation, so `perf stat` counters accompany it: cycles, instructions, IPC, cache
references, cache misses, and branch misses for both implementations.

## Timing methodology

`rdtscp` with a TSC frequency calibrated at startup against
`std::chrono::steady_clock`, with a documented fallback to `steady_clock` where the
TSC is unusable. TSC invariance is verified via `CPUID` leaf `0x80000007` bit 8,
and the harness fails loudly rather than silently reporting numbers derived from a
counter that changes rate with frequency. `rdtscp` is used rather than `rdtsc`
because it orders against prior loads, and the measured region is fenced so the
timestamp is not reordered into or out of it.

## Environment discipline

Every published result will carry the conditions that produced it:

- Thread pinned to an isolated core via `sched_setaffinity`. Recommended boot
  flags for a quiet core are documented alongside the results.
- A configurable warmup, run before recording starts, so that the arena is faulted
  in, the caches are warm, and no measurement includes a first-touch page fault.
- Turbo boost and frequency scaling detected and reported. If either is active,
  the result is printed with a warning rather than suppressed.
- CPU model, cache sizes at each level, compiler and version, exact compile flags,
  kernel version, and hugepage configuration printed with every result.
- Each configuration run multiple times, with run-to-run variance reported. A
  single run is a sample of one and the spread is part of the result.

## Reproduction

The exact command will be recorded here alongside the results table, so that a
reader can rerun it rather than trust it.

## Where this design is weak

This section will be populated with measured weaknesses at Phase 4. It exists now
because a benchmark document with no weaknesses section reads as marketing rather
than engineering, and committing to the section before seeing the numbers is the
only way to guarantee it gets written honestly.

The weaknesses already known from the design, to be confirmed or refuted by
measurement:

- **Band memory footprint.** A 65536 level band per side costs roughly 1.5 MiB per
  side for the level array alone, before the arena. That is far larger than a
  tree's working set for a book that only ever has a few hundred occupied levels.
  The bet is that direct indexing beats a tree despite the footprint, because only
  the occupied levels are ever touched and the bitmap avoids scanning the rest.
  Whether that bet holds when the book is sparse and the level array is cold is an
  empirical question, and it is the first thing I would push on if I were reading this.
- **Band rebasing cost.** Rebasing is rare but it is not cheap, and a hard trending
  market will trigger it. The tail of the latency distribution will show it, and
  the p99.99 number is where it becomes visible.
- **Overflow cold path.** Prices outside the band fall to a `std::map` backed path
  that is orders of magnitude slower. A workload that puts meaningful volume
  outside the band would invalidate the headline numbers, so the fraction of
  operations that took the cold path is reported alongside them.
- **Single symbol, single thread.** Throughput here is not a statement about a
  multi-symbol engine, and no attempt will be made to extrapolate it into one.
