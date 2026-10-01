# Shared benchmark renderer fixtures

These JSON files are the single source of truth for the cross-language parity of
the two benchmark renderers:

- `scripts/sync_benchmarks.py` (Python) renders the generated tables in
  `BENCHMARKS.md` and validates `site/data/bench/baseline.json`.
- `site/src/baseline.ts` (TypeScript) validates and formats the same artifact for
  the results site.

Both languages' test suites consume these fixtures so a divergence in validation
or formatting is caught mechanically rather than by two hand-mirrored test files
drifting apart.

They live here, outside `site/data`, on purpose: `site/data` is the Vite public
tree and everything in it ships as a published measurement. These are test inputs,
not measurements, so they must not be served by the site.

## Files

- `display_cases.json` - formatting parity. `format_ns` cases assert the timing
  and allocation display policy (decimal round-half-up to three decimals, then
  trailing zeros trimmed). `format_ratio` cases assert the ratio policy (one
  decimal, round-half-up from the decimal representation of the quotient).
- `validation_cases.json` - validator parity. Each case starts from `base`; then
  `replace` swaps the whole artifact (to test non-object top levels), or
  `set`/`delete` edit top-level keys, `rows` replaces the rows value, and `alloc`
  replaces `allocations_per_add`. `order_cases` assert row-order normalization.

## Consumers

- Python: `scripts/test_sync_benchmarks.py`
- TypeScript: `site/src/baseline.test.ts`

## Limitations

JSON cannot encode non-finite numbers (`Infinity`, `NaN`), so those rejection
cases are exercised by in-code tests in both suites rather than by these fixtures.

Parity is fixture-bounded, not proven for every possible double. The round-half-up
policy relies on Python's `str(float)` and JavaScript's `Number.prototype.toString`
producing the same shortest decimal, which holds for the accepted artifact values
and every case here but is not guaranteed universally. Rule: whenever a new value
enters `site/data/bench/baseline.json`, add a `display_cases.json` row for it so a
divergence is caught mechanically rather than shipped.
