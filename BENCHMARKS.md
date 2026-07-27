# Benchmarks

**Read the conditions before the numbers.** This document is organised so that is
possible: the Phase 1 first look comes first, labelled with everything wrong with
it, then the Phase 4 methodology exactly as it was written down before any of it
was measured, then the Phase 4 results.

That ordering is deliberate and worth keeping. The method was committed to the
repository before the measurements existed, so it could not be chosen
retroactively to suit a result.

**One caveat applies to everything here**: no isolated core was available on any
host this project can reach. Pinning is implemented and verified working on both
Windows and Linux, but pinning is not isolation. Comparative ratios and percentiles
up to p99.99 are sound. Maximum values are not, and are labelled as such wherever
they appear.

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
recorded as open rather than quietly dropped.

**Resolved in Phase 6, and the route there is the interesting part.** The Phase 4
baseline first made it worse rather than better: measured against the naive book on
an identical workload the flat book's best bid cost 10.2 ns against the tree's
5.38 ns, so it was not merely slower than its instruction count suggested, it was
slower than a `std::map` doing the same job.

The first guess above, that `std::optional` was the problem, was wrong. So was the
first fix attempt, which cached only the bitmap descent result and appeared to help
until an A/B against the unmodified code in the same session showed the apparent
gain was drift between measurement sessions. What worked was caching the final
answer including cold levels, which is what the tree does. Details under "The
comparative baseline against `std::map`" below.

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

**End-to-end replay latency**, now measured, results under "End to end replay
latency" below: per message latency across a full ITCH replay, recorded into an
HdrHistogram and reported as p50, p90, p99, p99.9, p99.99, and max in nanoseconds,
broken out by message type, plus throughput in messages per second. Averages will
not be reported alone. A mean hides the tail, and the tail is the number that
matters in this domain.

**Comparative baseline**, now measured, results under "The comparative baseline
against `std::map`" below: the naive `std::map<Price, std::list<Order>>` reference
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

### Timing methodology, built in `bench/harness.hpp`

`rdtscp` with a TSC frequency calibrated at startup against
`std::chrono::steady_clock`, with a documented fallback to `steady_clock` where the
TSC is unusable. TSC invariance is verified via `CPUID` leaf `0x80000007` bit 8,
and the harness fails loudly rather than silently reporting numbers derived from a
counter that changes rate with frequency. `rdtscp` is used rather than `rdtsc`
because it does not begin until prior instructions have retired, so it cannot float
above the measured region; an `lfence` on the other side of each read closes the
opposite direction.

Calibration takes seven 20 ms trials and uses the median, so a single scheduling
interruption during startup cannot skew every later number. It busy waits rather
than sleeping, because a sleep can return late by an unbounded amount and would
understate the tick rate.

### Environment discipline, built in `bench/harness.cpp`

Every result carries the conditions that produced it, printed by the program:

- Thread pinned through `sched_setaffinity` on POSIX and `SetThreadAffinityMask`
  on Windows. The Windows path is reported as the weaker guarantee it is: it stops
  migration but cannot stop other work sharing the core, because there is no
  `isolcpus` equivalent. Recommended boot flags for a genuinely quiet core are
  named in the pinning message itself.
- A configurable warmup, run before recording starts, so the caches are warm and
  the allocator has already faulted in pages of the size the measured runs ask
  for.
- Frequency scaling read from `intel_pstate/no_turbo` or `cpufreq/boost` where
  those exist, reported as unknown where they do not, and counted as a warning
  either way unless turbo is confirmed disabled.
- CPU brand from `CPUID`, hardware thread count, OS, compiler and version, build
  type, and the exact compile flags, which the build system hands to the code as a
  define so the printed flags cannot drift from the real ones.
- Every configuration run multiple times with run-to-run variance reported. A
  single run is a sample of one and the spread is part of the result.

**The rule throughout is to fail loudly.** A harness that quietly falls back to a
worse clock, or quietly fails to pin, and then prints a confident number is worse
than one that refuses to run, because the number outlives the caveat. Every
degraded condition is both recorded in the environment block and counted into a
warning total printed with the results.

## Phase 4 results

Phase 4 is complete. The nine case microbenchmark set, the `std::map` comparative
baseline, the HdrHistogram latency harness, and the measurement discipline all
exist and all ran.

One caveat survives everything and is not fixable on this hardware: **there is no
isolated core available**, on Windows or under WSL2. Pinning is implemented and
works, but pinning is not isolation. Ratios and percentiles up to p99.99 are sound;
`max` columns are not.

The microbenchmark figures below predate the harness and were taken through Google
Benchmark rather than through `bench/harness.hpp`, on the same machine and
toolchain as the Phase 1 table, GCC 16.1.0 at `-O3 -march=native`, 15 repetitions,
0.6 s minimum per repetition, median reported, with the run-to-run coefficient of
variation next to each figure so the reader can see which rows are solid.

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

### The comparative baseline against `std::map`

The headline of the phase. Both implementations receive an identical command
stream in the same order, in the same process, and store their events into the
same kind of vector, so event handling cannot tilt the result. The naive book is
`test/reference_book.hpp`, which matters: it is not a strawman written for this
benchmark, it is the oracle the differential test checks the engine against, so it
is the implementation whose agreement with the fast one is what the correctness
argument rests on.

Run with `ob_baseline_bench`, 9 repetitions, medians reported.

| Operation | Flat book | `std::map` book | Ratio |
| --- | --- | --- | --- |
| Add, resting | 26.9 ns | 73.5 ns | **2.7x faster** |
| Cancel | 10.3 ns | 56.0 ns | **5.4x faster** |
| Match, one level consumed | 38.7 ns | 65.2 ns | **1.7x faster** |
| Best bid query | **0.27 ns** | 4.48 ns | see below |

**The best price query was the one operation the flat book lost, and it has since
been fixed.** The history is worth keeping because the fix is only meaningful
against it. Phase 1 measured 13.4 ns and recorded it as unexplained. The Phase 4
baseline made the gap concrete: 10.2 ns against the tree's 5.38 ns, a genuine loss,
because `std::map` caches its extreme element while the bitmap performs three
dependent loads that cannot overlap.

The fix caches the final answer to `best()`, cold levels included, repaired lazily
when the best level empties. Measured back to back against the uncached code on the
same machine in the same session, the query went from 8.24 ns to 0.27 ns.

**That 0.27 ns needs its caveat stated, because on its own it is not credible.** It
is below one cycle, which is exactly what a benchmark that has been optimised away
also reports. `bm_query_noise_floor` exists to tell those apart: it runs the same
loop shape with no query at all and reports 0.17 ns. So the honest statement is
that the query costs about **0.09 ns above an empty loop, which is below what this
harness can resolve**, against the tree's 4.3 ns above the same floor. The reason it
pipelines so well is that it is now a single independent load per iteration, while
the tree's rbegin is a dependent chain that serialises.

The comparison is still worth publishing for the reason it always was: it did not
start out clean. Reporting the loss first, then fixing it, is the sequence that
makes the fix believable.

#### Where the ratio comes from

The largest single difference is allocation, and it is counted exactly rather than
inferred, through the `operator new` replacement in `test/alloc_counter.hpp`,
measured outside every timed region:

| | Allocations per add |
| --- | --- |
| Flat book | **0** |
| `std::map` book | **2.016** |

That figure decomposes completely, which is why it is worth more here than a
cache-miss sample would be. Each add allocates one `std::list` node for the
resting order and one `unordered_map` node for the id index, giving 2. Each of the
512 levels allocates one `std::map` node on the first add that reaches it, giving
512 over 32 768 adds, or 0.0156. Predicted total 2.0156 against 2.01599 measured.
The remaining twelve allocations across the whole run are the id index rehashing
its bucket array as it grows.

The flat book's zero is not an approximation either. The arena, the level array,
and the id map are all allocated once at construction, and the add path touches
none of them again.

#### The working set sweep, run on both

The instrument that stands in for the cache counters no host here provides. Queue
depth is held at 64 orders per level at every point, so the level count scales with
the order count and the algorithmic work per add is identical across the whole
sweep and between both arms. Whatever the curves show is memory behaviour.

| Live orders | Levels | Flat book | `std::map` book | Ratio |
| --- | --- | --- | --- | --- |
| 1 024 | 16 | 25.2 ns | 60.3 ns | 2.4x |
| 4 096 | 64 | 24.5 ns | 62.9 ns | 2.6x |
| 16 384 | 256 | 27.4 ns | 70.2 ns | 2.6x |
| 65 536 | 1 024 | 27.7 ns | 91.0 ns | 3.3x |

**The gap widens as the book grows**, from 2.4x to 3.3x, and that is the mechanical
explanation the ratio needed. The flat book rises 10 percent across a 64-fold
increase in live orders, because its orders are contiguous in an arena and its
levels are contiguous in an array. The naive book rises 51 percent across the same
range, because every order is a separately allocated node reached by pointer, so a
larger book means a more scattered heap and a worse hit rate on every traversal.
Allocation explains the constant factor; scattering explains why the factor grows.

An earlier version of this sweep held the level count fixed at 512 instead of
scaling it, and produced a naive curve that fell as the working set grew. That was
not a cache effect and was not believable as one. With a fixed level count, a 1 024
order book creates a new level on half its adds while a 65 536 order book creates
one on 1 in 128, and a new level is a `std::map` node allocation, so the sweep was
varying the algorithmic work rather than holding it constant. Recorded because the
first result was wrong in a way that looked plausible.

#### Reproduction

```bash
cmake --preset release
cmake --build --preset release
./out/build/release/ob_baseline_bench --benchmark_repetitions=9 --benchmark_report_aggregates_only=true
```

### End to end replay latency

`ob_latency_bench` replays a real ITCH capture and times every message
individually, recording into an HdrHistogram. This is the distribution the phase
was built to produce, and it is reported as percentiles because a mean hides the
tail and the tail is the number that matters here.

Conditions, all captured by the program itself and printed with every run:

| | |
| --- | --- |
| Clock | invariant TSC, CPUID leaf `0x80000007` bit 8 verified set, calibrated to 3.11039 ticks/ns against `steady_clock` |
| Core | pinned to cpu 2 through `SetThreadAffinityMask` |
| Arena | 131 072 slots, 5 MiB |
| Warmup | 2 unrecorded replays, then 5 measured |
| Input | `data/qqq_slice.itch`, 183 954 QQQ messages per run |
| Toolchain | GCC 16.1.0, `-O3` |

| Message type | count | p50 | p90 | p99 | p99.9 | p99.99 | mean |
| --- | --- | --- | --- | --- | --- | --- | --- |
| add order (A/F) | 463 525 | 42 | 126 | 165 | 262 | 5 147 | 61.2 |
| executed (E/C) | 4 935 | 45 | 60 | 104 | 189 | 399 | 47.9 |
| cancel (X) | 5 310 | 40 | 51 | 65 | 165 | 177 | 42.4 |
| delete (D) | 422 405 | 37 | 49 | 63 | 117 | 244 | 39.4 |
| replace (U) | 22 435 | 62 | 145 | 189 | 288 | 430 | 75.9 |
| trade (P/Q/B) | 300 | 32 | 41 | 46 | 48 | 48 | 34.0 |
| system/directory | 20 | 38 | 186 | 737 | 737 | 737 | 113.5 |
| parsed, discarded | 840 | 31 | 32 | 46 | 183 | 263 | 31.0 |
| **all** | **919 770** | **41** | **79** | **158** | **227** | **549** | **51.3** |

Nanoseconds, three significant figures across a 1 ns to 1 s range.

**Every figure includes 26.68 ns of timer overhead**, measured by the harness as
the median of 4 096 back to back timestamp reads and printed next to the results.
It is deliberately not subtracted: removing a noisy per sample estimate would
corrupt the tail, which is the part worth reading. Subtract it mentally when
comparing against a microbenchmark, which puts a delete at roughly 11 ns and an
add at roughly 15 ns of actual work.

The shape is what the design predicts. Deletes and cancels are the cheapest
mutations, an add costs more because it allocates a slot and may occupy a level,
and a replace is the most expensive because it is a delete and an add in one
message. The `p99.99` column for adds at 5 147 ns is the band rebase becoming
visible, which BENCHMARKS.md predicted before it was measured.

#### Latency and throughput cannot be measured in the same pass

The harness runs them separately, and the reason is quantitative rather than
stylistic. Two timestamp reads plus two fences cost about 27 ns against a message
that costs about 25 ns, so instrumenting every message more than doubles the work.

| | Messages per second |
| --- | --- |
| Measured inside the instrumented loop | 10.9 M |
| Measured on separate uninstrumented runs | **37.8 M**, run to run CV 5.99 % |

A throughput number taken from the instrumented loop understates the engine by
3.5 times. It would also have been the easiest possible number to publish by
accident, since it falls out of the same run that produces the percentiles.

#### The same code on Linux, as a cross check

The harness was built and run under WSL2 with GCC 13.3.0, pinning through
`sched_setaffinity` instead of `SetThreadAffinityMask`. This exercises a different
compiler, a different OS, and the POSIX branches of both the memory mapping and
the pinning code.

| | Windows, GCC 16.1.0 | Linux, GCC 13.3.0 |
| --- | --- | --- |
| TSC calibration | 3.11039 ticks/ns | 3.11038 ticks/ns |
| add p50 | 42 | 46 |
| delete p50 | 37 | 38 |
| replace p50 | 62 | 65 |
| all p50 | 41 | 41 |
| all p99 | 158 | 173 |
| Throughput | 37.8 M msg/s | 48.2 M msg/s |

Two independently calibrated TSC frequencies agreeing to five digits, and
percentiles agreeing within a few nanoseconds across two compilers and two
operating systems, is the evidence that the measurement itself is sound rather
than an artifact of one toolchain. The throughput gap is the one real difference
and it is not investigated here.

#### What is still wrong with these numbers

The harness reports its own degraded conditions rather than hiding them, and on
this host there is one that it cannot fix:

- **No core isolation.** Pinning stops the thread migrating between performance
  and efficiency cores. It does not stop the OS scheduling other work onto the
  same core, because Windows has no `isolcpus` equivalent and WSL2 is a guest.
  **This makes the `max` column meaningless**: a 171 775 ns maximum is a
  scheduling event, not a property of the book. `p99.9` and `p99.99` are the tail
  figures worth reading until a machine with an isolated core is available.
- **Frequency scaling is unknown.** Not readable on Windows, and not exposed under
  WSL2 either, so it is reported as unknown and counted as a warning rather than
  quietly passed over.

#### Reproduction

```bash
./out/build/release/ob_latency_bench --file data/qqq_slice.itch --symbol QQQ --cpu 2 --warmup 2 --runs 5
```

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
