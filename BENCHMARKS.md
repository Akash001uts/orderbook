# Benchmarks

**The headline results in this document are not yet trustworthy.** The disciplined
harness lands in Phase 4. What is published below is a first look from Phase 1,
labelled with everything wrong with it, plus the methodology Phase 4 will
implement, recorded before that measurement is taken so the method cannot be chosen
retroactively to suit a result.

An unmeasured performance claim is worthless, and a measured claim without its
methodology is not much better. Both the numbers and the conditions that produced
them are deliverables here.

## Phase 1 first look

**Measurement conditions, all of which are wrong in ways that matter.**

| | |
| --- | --- |
| CPU | Intel Core Ultra 5 238V, 8 cores, reported at 3110 MHz |
| Caches | L1d 48 KiB per core, L2 2560 KiB per core, L3 8192 KiB |
| OS | Windows 11, no core isolation, no frequency pinning |
| Toolchain | GCC 16.1.0, `-O3 -march=native -fno-omit-frame-pointer` |
| Harness | Google Benchmark 1.9.4, 5 repetitions, 1.5 s minimum per repetition |

Every one of the following is a reason not to quote these numbers:

- **No core pinning.** This is a hybrid CPU with performance and efficiency cores.
  A thread the scheduler moves between the two changes speed for reasons that have
  nothing to do with the code. Windows offers no equivalent of `isolcpus`.
- **No frequency control.** Turbo and scaling are both active and undetected.
- **Run to run variance is large.** The coefficient of variation reaches 20 percent
  on the add benchmark. Anything below a factor of two difference between two rows
  here is inside the noise.
- **No hardware cache counters.** `perf stat` has no Windows equivalent, so the
  cache behaviour below is inferred from a working set sweep rather than measured
  from the PMU.

### Operation costs

Arena sized to 65536 orders, a busy but realistic single symbol depth. CPU time,
median of 5 repetitions.

| Operation | ns | Notes |
| --- | --- | --- |
| Order id lookup | 2.3 | Load factor 0.50, mean probe count 1.0 |
| Cancel from level head | 18.7 | Includes lookup, unlink, bitmap clear, arena free |
| Add plus cancel round trip, all L1 resident | 18.8 | Best case, not comparable to the row below |
| Add to an existing level | 22.8 | Coefficient of variation 20 percent |
| Best bid query | 13.4 | Higher than the three loads and three bit operations it performs, unexplained, see below |

### Working set sweep

The same add operation, swept over arena capacity. The algorithmic work is
identical at every point, so the entire shape of this curve is cache behaviour.
This is the experiment that stands in for `perf stat` cache miss counters on a host
that has no PMU access.

| Arena capacity | Arena size | ns per add |
| --- | --- | --- |
| 4 096 | 0.16 MiB | 18.3 |
| 16 384 | 0.63 MiB | 18.8 |
| 65 536 | 2.5 MiB | 36.0 |
| 262 144 | 10 MiB | 102 |
| 1 048 576 | 40 MiB | 130 |

L2 on this machine is 2.5 MiB, and the curve breaks exactly there. The
interpretation is that the book's own work is about 18 ns and everything above
that is memory latency.

This sweep exists because the first version of the benchmark used a 2^20 slot
arena and reported 54 ns for an add. That number was a DRAM latency wearing an
add's name. The default arena size was reduced to 65536 as a result, which is
documented in DESIGN.md as a cache decision rather than a capacity one.

### ITCH pipeline throughput

Synthetic file, 500 000 messages, 16.5 MiB, roughly 10 percent belonging to decoy
symbols that replay filters out. GCC 16.1.0 `-O3 -march=native`, median of 3
repetitions. Same caveats as everything above: no core pinning, no frequency
control.

| Arm | Throughput | Per message | What it includes |
| --- | --- | --- | --- |
| Parse only | 312 M msg/s, 9.7 GiB/s | 3.2 ns | Framing, length cross check, and two header field decodes |
| Parse and replay | 15.7 M msg/s, 499 MiB/s | 64 ns | The above plus the book mutation, 450 k of 500 k messages applied |

The two arms are reported separately on purpose. The parse-only arm is what a
filtered replay does to the majority of messages in a real multi-symbol capture:
read the type and the locate, then discard. Publishing only the combined figure
would make it impossible to tell which half a change affected.

The 3.2 ns parse figure is the strongest evidence that byte-by-byte big endian
assembly costs nothing next to a struct cast: at 3.1 GHz that is about ten cycles
per message including the framing arithmetic and the length table lookup.

Generated message mix, from `itch_gen --messages 200000`:

| Type | Count | Share |
| --- | --- | --- |
| `A`/`F` add order | 85 886 | 42.9 % |
| `D` delete | 42 441 | 21.2 % |
| `E`/`C` executed | 23 253 | 11.6 % |
| decoy symbols | 19 957 | 10.0 % |
| `X` cancel | 12 321 | 6.2 % |
| `U` replace | 8 920 | 4.5 % |
| `P` trade | 3 624 | 1.8 % |
| `Q` cross | 1 824 | 0.9 % |
| `B` broken | 1 770 | 0.9 % |

### The synthetic mix is not the real mix

Every throughput figure above was measured on a synthetically generated file. A real
NASDAQ capture has a materially different message distribution, so these numbers do
not transfer, and Phase 4 measures both.

| Type | Synthetic | Real, 2019-12-30 |
| --- | --- | --- |
| `A`/`F` add | 42.9 % | 44.9 % |
| `D` delete | 21.2 % | 30.4 % |
| `E`/`C` executed | 11.6 % | 1.1 % |
| `U` replace | 4.5 % | 7.6 % |
| `X` cancel | 6.2 % | 4.7 % |
| `I` net order imbalance | 0 % | 9.0 % |

The generator overstates executions by roughly ten times and emits no imbalance
messages at all. Executions and cancels are the expensive book operations, so a
synthetic throughput number is optimistic about how much book work a real feed
demands and pessimistic about how much pure parsing it demands. Phase 4 reports the
real capture separately rather than adjusting the synthetic figure.

### What is not yet explained

The best bid query at 13.4 ns is slower than its instruction count justifies. It
performs three dependent loads into structures totalling 8 KiB per side, which
should be resident, plus three bit manipulation instructions. Candidate
explanations are the `std::optional` return being materialised rather than kept in
registers, and the benchmark's `DoNotOptimize` barrier forcing a store. This is
recorded as open rather than quietly dropped, and Phase 4 resolves it.

### Reproduction

```bash
cmake --preset release
cmake --build --preset release
./out/build/release/ob_micro_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## What Phase 4 adds

### What will be measured

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
numbers published.

The speedup ratio is only meaningful alongside a mechanical explanation, and that
explanation has to be built from instruments this project can actually read.
`perf stat` counters are available on none of the hosts it runs on: Windows has no
equivalent, WSL2 is a hypervisor guest that does not expose the PMU, and
GitHub-hosted runners do not expose it either. Three substitutes carry the
explanation instead.

| Instrument | What it explains | How it is measured |
| --- | --- | --- |
| Allocation counts | Node allocation, the largest single structural difference between the two implementations | The `operator new` replacement in `test/alloc_counter.hpp`, already built to assert that the hot path does not allocate |
| Working set sweep | Memory behaviour, which is what the absent cache counters would have shown | The Phase 1 technique run on both implementations: hold the algorithmic work constant, vary the book size, and read the divergence between the two curves |
| Pointer hops per operation | Why the sweep curves separate where they do | Counted structurally from the code, stated per operation, then checked against the sweep |

The allocation instrument is exact rather than sampled, which is worth more here
than a cache miss count. The reference book allocates a `std::map` node for every
newly occupied level, a `std::list` node for every resting order, and an
`unordered_map` node for every live order id. The flat book allocates nothing at
all once constructed. That difference is countable rather than inferred, it is
already asserted on in the test suite, and it accounts for more of the ratio than
any single cache statistic would.

`perf stat` counters remain the better artifact and are not a blocker. If a
bare-metal Linux machine becomes available, they are added alongside these three
rather than in place of them.

### Timing methodology

`rdtscp` with a TSC frequency calibrated at startup against
`std::chrono::steady_clock`, with a documented fallback to `steady_clock` where the
TSC is unusable. TSC invariance is verified via `CPUID` leaf `0x80000007` bit 8,
and the harness fails loudly rather than silently reporting numbers derived from a
counter that changes rate with frequency. `rdtscp` is used rather than `rdtsc`
because it orders against prior loads, and the measured region is fenced so the
timestamp is not reordered into or out of it.

### Environment discipline

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

### Reproduction command

The exact command will be recorded here alongside the results table, so that a
reader can rerun it rather than trust it.

## Phase 4 results so far, the corrected comparisons

The nine case microbenchmark set is complete. The measurement discipline around it
is not: core pinning, warmup, turbo detection, and environment capture are still
ahead in this phase. **Every caveat from the Phase 1 first look still applies to
the absolute numbers below**, and they are published as comparisons rather than as
figures precisely because the comparisons survive the noise that the absolute
values do not.

Conditions are the same machine and toolchain as the Phase 1 table above, GCC
16.1.0 at `-O3 -march=native`, Google Benchmark, 15 repetitions, 0.6 s minimum per
repetition, median reported, with the run-to-run coefficient of variation next to
each figure so the reader can see which rows are solid.

### The occupancy bitmap transition, now measured correctly

This resolves the second of the three open items carried in from Phase 1. The old
comparison was invalid, and the new one is a paired design: both arms hold the book
at an identical live order count, perform one add and one cancel per iteration,
reuse the same arena slot, and walk the same levels. The only difference is a
sentinel order that keeps the toggle level occupied in one arm, so that no bitmap
bit ever changes there.

| Arm | ns | CV | What differs |
| --- | --- | --- | --- |
| Transition included | 9.50 | 2.30 % | Level goes empty to occupied and back, one bit set and one bit cleared |
| Transition excluded | 8.29 | 3.52 % | Sentinel keeps the level occupied throughout, no bit changes |

The difference is about 1.2 ns for the pair, so roughly 0.6 ns or two cycles per
transition. The gap is three to four standard deviations wide, so unlike the Phase
1 attempt it is a real difference rather than noise.

**The direction is the point.** The Phase 1 measurement had the arm doing strictly
more work coming out faster, which is impossible and is what exposed the flaw. Here
the arm that touches the bitmap is the slower one, by an amount that a bit set, a
bit clear, and the surrounding occupancy bookkeeping can actually account for.

### Cancel does not care where the order sits in its queue

Each level holds 64 orders. A design that stored levels as a list and searched them
would make the tail roughly 64 times the cost of the head.

| Position in queue | ns | CV |
| --- | --- | --- |
| Head | 10.0 | 1.45 % |
| Middle | 11.6 | 6.29 % |
| Tail | 11.2 | 1.24 % |

Tail over head is 1.12, not 64. That is the intrusive-link claim demonstrated: the
id map goes straight to the arena slot and the links splice it out without a walk.

The honest wrinkle is that the three are not exactly equal, and the middle arm is
about 1.5 ns slower than the head arm, which is outside the noise. That is not an
algorithmic cost, because an algorithmic walk would show up as a factor rather than
as 15 percent. It is the access pattern: the middle arm cancels a scattered id
sequence, so its id map probes and arena touches predict and prefetch worse than
the head arm's more ordered sequence. Reported rather than smoothed over.

### The remaining new cases

| Operation | ns | CV | Notes |
| --- | --- | --- | --- |
| Modify down | 4.26 | 1.76 % | In-place quantity reduction, queue position preserved. The cheapest operation the book has, as expected: a lookup and a field write, no link or bitmap work |
| Aggressive order crossing one level | 38.1 | 22.2 % | Consumes one resting order and empties its level. The high variance is the unpinned hybrid CPU and is exactly what the Phase 4 discipline is meant to remove |
| Aggressive order crossing ten levels | 351 | 2.96 % | Consumes ten levels in one command |

Ten levels costs 9.2 times one level, so the marginal cost of each additional level
is about 35 ns against a first level of 38 ns. Crossing is therefore very close to
linear in levels consumed, with only a small fixed per-command component, which is
what a level walk with a bitmap-driven next-level lookup should produce.

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
