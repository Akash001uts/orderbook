// The figures this page states rather than reads.
//
// Everything per symbol comes out of JSON. These do not, because they describe
// measurements this site does not ship artifacts for: a microbenchmark harness
// run, a sweep across arena sizes, a mutation exercise. Exactly two such tables
// are allowed, the hero and the measurement story, on the condition that each
// cites the section of BENCHMARKS.md it came from. Each does.
//
// If a number here ever disagrees with that document, that document is right and
// this file is stale.

export const REPO_URL = 'https://github.com/Akash001uts/orderbook'

export interface Citation {
  document: string
  section: string
}

/** BENCHMARKS.md, "The comparative baseline against std::map". The numbers
 *  themselves are read from site/data/bench/baseline.json rather than stated here,
 *  because that artifact is the single source the document and this site share. */
export const BASELINE_CITATION: Citation = {
  document: 'BENCHMARKS.md',
  section: 'The comparative baseline against std::map',
}

// Footnote about allocations is generated from the baseline artifact in the hero
// panel so it cannot drift from the published figures. These are the caveats that
// do not depend on a specific number.
export const HERO_FOOTNOTES: string[] = [
  'The best price query was the one operation the flat book originally lost, 10.2 ns against the tree’s 5.38 ns, because std::map caches its extreme element while a bitmap descent is three dependent loads. Caching the final answer and repairing it lazily fixed it, and it is now below what the harness can resolve. The loss is published for the same reason it is there: reporting it first is what makes the fix believable.',
  'Ratios are the headline rather than the absolute nanoseconds. This machine drifted 2.6x on the same binary across one afternoon, so every comparison is taken from a paired run measured back to back in one process.',
  'There is no isolated core. Pinning is implemented and verified on both Windows and Linux, but pinning is not isolation, so maximum values are scheduling events rather than properties of the book. Percentiles to p99.99 are sound.',
]

export interface ArenaCurveRow {
  capacity: number
  workingSet: string
  quiet: number
  loaded: number
}

/** BENCHMARKS.md, "The arena curve, and why it is published twice". */
export const ARENA_CURVE: ArenaCurveRow[] = [
  { capacity: 4096, workingSet: '0.16 MiB', quiet: 12.6, loaded: 15.4 },
  { capacity: 16384, workingSet: '0.63 MiB', quiet: 15.4, loaded: 20.0 },
  { capacity: 18432, workingSet: '0.70 MiB', quiet: 15.9, loaded: 22.6 },
  { capacity: 65536, workingSet: '2.5 MiB', quiet: 16.2, loaded: 26.9 },
  { capacity: 262144, workingSet: '10 MiB', quiet: 29.2, loaded: 67.6 },
  { capacity: 1048576, workingSet: '40 MiB', quiet: 71.8, loaded: 132 },
]

export const ARENA_CITATION: Citation = {
  document: 'BENCHMARKS.md',
  section: 'The two capacity constants, derived rather than chosen',
}

export interface MutationRow {
  injected: string
  caught: string
  command: number
}

/** DESIGN.md, "Validating the test by mutation". */
export const MUTATIONS: MutationRow[] = [
  { injected: 'Crossing predicate changed from <= to <', caught: 'event stream', command: 3 },
  {
    injected: 'Partial fill skips the level aggregate update',
    caught: 'aggregate quantity',
    command: 9,
  },
  {
    injected: 'A quantity reduction requeues instead of holding its place',
    caught: 'queue order',
    command: 399,
  },
]

export const MUTATION_CITATION: Citation = {
  document: 'DESIGN.md',
  section: 'Validating the test by mutation',
}

/** Counts that describe the repository rather than a measurement. */
export const PROJECT = {
  // Registered ctest count as of 2026-09-23 (ctest --preset debug); 161 pass and
  // one differential regression replay is skipped until a fixture is committed.
  tests: 162,
  ciJobs: 16,
  bugLogEntries: 8,
  compilers: 'GCC 16.1.0 and Clang 22.1.8',
  ciCompilers: 'GCC 13 and Clang 18',
  presets: 'debug, release, relwithdebinfo',
  sanitizers: 'asan, ubsan, tsan',
  captureDate: '2019-12-30',
  captureSymbols: '8 906',
  captureMessages: '268 744 780',
}

export const LINKS: Array<{ label: string; file: string; blurb: string }> = [
  {
    label: 'DESIGN.md',
    file: 'DESIGN.md',
    blurb: 'Every decision, the alternatives rejected, and a log of eight bugs the tests caught.',
  },
  {
    label: 'BENCHMARKS.md',
    file: 'BENCHMARKS.md',
    blurb: 'The measurement discipline, the baseline comparison, and what is still wrong with the numbers.',
  },
  {
    label: 'STRATEGY.md',
    file: 'STRATEGY.md',
    blurb: 'The market maker, queue position estimation, P&L attribution, and the markout reading.',
  },
  {
    label: 'ROADMAP.md',
    file: 'ROADMAP.md',
    blurb: 'What is deliberately kept cheap, and why.',
  },
]

export const ARCHITECTURE = String.raw`
   ITCH 5.0 bytes                  mmap, zero copy
        |
        v
   +----------------+   MessageView    +------------------+
   | parser         | ---------------> | ReplayDriver     |
   | framing, decode|                  | locate filter    |
   +----------------+                  +--------+---------+
                                                |
                                                v
   +--------------------------------------------------------+
   | Book<BandLevels>                                        |
   |                                                         |
   |  band: flat array, direct indexed by tick               |
   |  bitmap: occupied levels, best price in three loads     |
   |  arena: orders in one contiguous block, no allocation   |
   |  id map: open addressing, order id to arena index       |
   |  cold: std::map, out of band prices only, out of line   |
   +----------------------------+----------------------------+
                                |
                                v
   +----------------+                  +------------------+
   | Engine         |  ExecutionEvent  | MarketMaker      |
   | five order     | ---------------> | queue position   |
   | types, STP     |                  | P&L, markouts    |
   +----------------+                  +------------------+
`
