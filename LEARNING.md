# What I learned building this

I started this project to understand how an exchange actually matches orders, and
to find out what "low latency C++" means once you have to measure it rather than
just say it. I'm a Data Analytics student, and most of what's in here (custom
memory layouts, binary exchange feeds, careful benchmarking) was new to me.

This file is the honest version of the journey: what each phase was meant to teach
me, what went wrong, and what I'd tell myself if I started again. The technical
reasoning lives in [DESIGN.md](DESIGN.md) and the numbers live in
[BENCHMARKS.md](BENCHMARKS.md). The commit history has the detail of each change.

## Phase 0: scaffold, types, and CI

**Goal.** Set up the build, a type layer, and CI before writing any engine code.

- **Strong types catch real mistakes.** Making `Price`, `Quantity` and `OrderId`
  distinct types means passing a quantity where a price belongs no longer compiles.
  I learned to test that something *doesn't* compile with a `requires` expression,
  since `static_assert` can't say "this must fail".
- **Prices should never be floating point.** Fixed-point integers, converted only at
  the API boundary, keep the division off the hot path and the rounding out of the
  book.
- **A green local build proves less than it looks.** CI caught failures my laptop
  couldn't: GCC 13 rejected code GCC 16 accepted, and ThreadSanitizer's runtime
  clashed with my allocation counter. I realised CI is a second opinion, not a
  formality.

## Phase 1: the flat order book

**Goal.** Build the core data structure: a flat array of price levels indexed by
tick, an order arena, an id map, and a bitmap for the best price.

- **Benchmarks lie if the working set is wrong.** My first add benchmark said 54 ns.
  It was measuring DRAM, because a 2^20 slot arena is 64 MiB against an 8 MiB L3.
  Sweeping the arena size showed the curve breaking exactly at the L2 size, which
  made cache effects something I could see rather than just read about.
- **Invariants I "knew" were true weren't.** I assumed band rebasing kept every cold
  level worse than the band's best. It can't, because two prices further apart than
  the band is wide can't both fit. Writing the bug up in DESIGN.md made me
  understand it properly.
- **32-bit indices instead of pointers** keep an order at 40 bytes, and a generation
  counter makes a stale index detectable without widening anything.

## Phase 2: the matching engine and the differential test

**Goal.** Add matching and every order type, and prove it is correct.

- **A fast data structure is common; proof that it is correct is not.** I wrote a
  deliberately naive `std::map` reference book and compared full state after every
  single randomised command. This became the most valuable thing in the project.
- **Test the test.** I injected three bugs on purpose and checked each was caught by
  a different comparison. Before that, I was only assuming the test worked.
- **One matching path, not two.** Making a market order a limit at the most
  aggressive price meant there was only one place for the semantics to drift.
- **My own assumption was wrong, not the code.** A test I wrote to build a deep book
  only produced two levels, because uniform random placement sweeps the book rather
  than building depth.

## Phase 3: parsing real exchange data

**Goal.** Read NASDAQ TotalView-ITCH 5.0 and replay it into the engine.

- **Casting bytes to a struct is undefined behaviour,** even though it works on x86.
  ITCH fields are big-endian and unaligned, so I assembled every field byte by byte.
  The compiler turns that into the same instructions anyway.
- **Synthetic data only proves self-consistency.** Replaying a real capture was what
  showed the parser and the live feed agree: zero unknown message types across 12
  million messages, and a reconstructed QQQ close that matched the real price.
- **Real data surprises you.** Five of 92 705 QQQ adds were priced off a penny,
  because sub-penny prices exist. The tick size turned out to be a property of the
  data, not the code.
- **Filter by locate code, not symbol.** Cancels and executions carry no symbol, so a
  string filter would silently drop every modification.

## Phase 4: measuring properly

**Goal.** Microbenchmarks, a latency harness, and a fair baseline comparison.

- **Measure latency and throughput separately.** Two timestamps per message cost
  about as much as the message, so the instrumented loop reported 10.9M msg/s
  against 37.8M uninstrumented. That would have been an easy wrong number to publish.
- **My flat book lost one benchmark.** `std::map` caches its extreme element, so it
  answered "best bid" faster than my three-load bitmap. I reported it, then fixed it
  by caching the answer and repairing it lazily.
- **A result below one cycle needs a noise floor.** 0.27 ns looks like a huge win, but
  it is also what an optimised-away benchmark reports. I added an empty-loop
  benchmark so the claim could be checked.
- **Machines drift.** The same binary gave 23.5 ns and 60.5 ns an hour apart. Since
  then I only trust back-to-back comparisons, and I treat ratios as the durable part.
- **No performance counters were available** on any machine I had, so I counted
  allocations exactly instead. The 2.064 allocations per add for `std::map`
  decomposes exactly into list, hash and map nodes.

## Phase 5: a market maker

**Goal.** Run a simple quoting strategy inside the same engine as the replayed market.

- **The fill model decides everything.** I wrote it down before looking at any P&L,
  and refused to treat a passive venue add crossing my quote as a fill, because that
  is the most flattering assumption available.
- **A risk limit that doesn't limit risk.** My position check used the current
  position instead of the position after a full fill, so the book could settle 99
  shares over the limit.
- **The horizon decides what a metric can see.** At ten seconds my fills looked good.
  At five minutes the markout turned negative. I had to withdraw a conclusion I'd
  already written, and I kept the correction visible.
- **Latency went the opposite way to my prediction.** I expected fills to drop as
  reaction latency rose. They rose instead, because a quote I'd decided to pull stays
  fillable during the delay.

## Phase 6: documentation and sizing

**Goal.** Make the reasoning readable, and remove anything that wasn't earning its place.

- **Duplicated documentation goes stale.** The same rationale lived in three places,
  and one copy was wrong by five phases. One source of truth per claim.
- **Defaults should come from data.** I sized the arena from one symbol, then
  checked five and found peak depth varies 62-fold (IWM 434 orders, AAPL 27 097).
  The default I'd derived from QQQ alone would have rejected orders on AAPL.
- **Dead code hides in good intentions.** Six fields and accessors I wrote "for later"
  were never read, two of them updated on every message.

## The results site

**Goal.** Show the results without asking anyone to build C++.

- **Stdout is not an interface.** The tools write versioned JSON, and CI regenerates
  it from the committed data slice and diffs the bytes, so a code change that moves
  a published number fails the build.
- **Text mode on Windows changes the bytes.** The same artifact was a different file
  depending on which OS wrote it until I opened streams in binary mode.

## If I started again

- Write the differential test on day one, before the optimised book.
- Never quote a benchmark number that wasn't measured back to back with what it's
  compared against.
- Check assumptions against real data early. The real capture found things no
  synthetic test could.
