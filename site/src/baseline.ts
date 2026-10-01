// The canonical std::map comparison contract and its validator.
//
// Kept in its own module, free of any DOM or fetch reference, so it can be
// imported both by the browser data layer and by a plain Node test. The site
// binds to these fields by name, so a file that parses but does not satisfy this
// shape is as much a load failure as one that is missing: validateBaseline throws,
// and the caller shows the visible error state rather than rendering blank.
//
// The checks mirror scripts/sync_benchmarks.py exactly, so the document renderer
// and the site agree on what a valid artifact is. If a field here disagrees with
// site/data/SCHEMA.md, that document wins.

export type BaselineInterpretation = 'faster' | 'below_resolution'

export interface BaselineRow {
  id: string
  label: string
  flat_ns: number
  naive_ns: number
  interpretation: BaselineInterpretation
}

export interface BaselineArtifact {
  schema: number
  tool: string
  version: string
  benchmark_command: string
  repetitions: number
  statistic: string
  rows: BaselineRow[]
  query_noise_floor_ns: number
  allocations_per_add: { flat: number; naive: number }
}

export const REQUIRED_BASELINE_ROWS = ['add', 'cancel', 'match', 'best_bid']

const INTERPRETATIONS: BaselineInterpretation[] = ['faster', 'below_resolution']

export class BaselineError extends Error {}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function requireString(record: Record<string, unknown>, field: string): string {
  const value = record[field]
  // Reject whitespace-only as well as empty: a metadata field or label that is
  // only spaces would otherwise reach public output as a blank cell.
  if (typeof value !== 'string' || value.trim().length === 0) {
    throw new BaselineError(`baseline.json: ${field} must be a non-empty, non-whitespace string`)
  }
  return value
}

function requireFinite(
  record: Record<string, unknown>,
  field: string,
  { positive }: { positive: boolean },
): number {
  const value = record[field]
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new BaselineError(`baseline.json: ${field} must be a finite number`)
  }
  if (positive ? value <= 0 : value < 0) {
    throw new BaselineError(
      `baseline.json: ${field} must be ${positive ? 'positive' : 'non-negative'}, found ${value}`,
    )
  }
  return value
}

// Accepts genuinely unknown input, so a corrupt or truncated file is rejected
// rather than trusted because TypeScript's compile-time type said it was fine.
export function validateBaseline(input: unknown): BaselineArtifact {
  if (!isObject(input)) {
    throw new BaselineError('baseline.json: top level must be an object')
  }

  if (input.schema !== 1) {
    throw new BaselineError(`baseline.json: expected schema 1, found ${String(input.schema)}`)
  }

  requireString(input, 'tool')
  requireString(input, 'version')
  requireString(input, 'benchmark_command')
  requireString(input, 'statistic')
  const repetitions = input.repetitions
  if (typeof repetitions !== 'number' || !Number.isInteger(repetitions) || repetitions <= 0) {
    throw new BaselineError('baseline.json: repetitions must be a positive integer')
  }

  const rows = input.rows
  if (!Array.isArray(rows) || rows.length === 0) {
    throw new BaselineError('baseline.json: rows must be a non-empty array')
  }

  const ids = new Set<string>()
  const validated: BaselineRow[] = []
  for (const row of rows) {
    if (!isObject(row)) {
      throw new BaselineError('baseline.json: every row must be an object')
    }
    const id = requireString(row, 'id')
    if (ids.has(id)) {
      throw new BaselineError(`baseline.json: duplicate row id ${id}`)
    }
    ids.add(id)
    const label = requireString(row, 'label')
    const flat_ns = requireFinite(row, 'flat_ns', { positive: true })
    const naive_ns = requireFinite(row, 'naive_ns', { positive: true })
    const interpretation = row.interpretation
    if (interpretation !== 'faster' && interpretation !== 'below_resolution') {
      throw new BaselineError(`baseline.json: row ${id} has unknown interpretation`)
    }
    // A faster row renders a ratio, so the flat book must genuinely be faster;
    // otherwise the hero would show a misleading "0.8x faster" from an
    // interpretation that no longer matches the numbers.
    if (interpretation === 'faster') {
      if (naive_ns <= flat_ns) {
        throw new BaselineError(
          `baseline.json: row ${id} is marked faster but flat is not below naive`,
        )
      }
      // The derived ratio must itself be finite and positive. Individually finite
      // operands can still overflow to Infinity (a tiny flat against a huge naive),
      // which would otherwise reach the renderer. JavaScript yields Infinity here
      // where Python yields inf; both are the same rejection.
      const derived = naive_ns / flat_ns
      if (!Number.isFinite(derived) || derived <= 0) {
        throw new BaselineError(`baseline.json: row ${id} has a non-finite ratio`)
      }
    }
    // best_bid is an accepted below-resolution result. Pinning its interpretation
    // keeps it from being promoted to a headline ratio without a reviewed schema
    // or measurement change.
    if (id === 'best_bid' && interpretation !== 'below_resolution') {
      throw new BaselineError(`baseline.json: row best_bid must be below_resolution`)
    }
    validated.push({ id, label, flat_ns, naive_ns, interpretation })
  }

  // Require exactly the supported set: every required id present, and no others,
  // so an unsupported operation cannot silently appear in public output.
  for (const id of REQUIRED_BASELINE_ROWS) {
    if (!ids.has(id)) {
      throw new BaselineError(`baseline.json: missing required row id ${id}`)
    }
  }
  for (const id of ids) {
    if (!REQUIRED_BASELINE_ROWS.includes(id)) {
      throw new BaselineError(`baseline.json: unknown row id ${id}`)
    }
  }

  // Normalize to the canonical order so a permuted-but-valid artifact renders
  // identically. REQUIRED_BASELINE_ROWS is the one authoritative order.
  const byId = new Map(validated.map((row) => [row.id, row]))
  const ordered = REQUIRED_BASELINE_ROWS.map((id) => byId.get(id)!)

  requireFinite(input, 'query_noise_floor_ns', { positive: false })

  const alloc = input.allocations_per_add
  if (!isObject(alloc)) {
    throw new BaselineError('baseline.json: allocations_per_add must be an object')
  }
  const flat = requireFinite(alloc, 'flat', { positive: false })
  const naive = requireFinite(alloc, 'naive', { positive: false })

  return {
    schema: 1,
    tool: input.tool as string,
    version: input.version as string,
    benchmark_command: input.benchmark_command as string,
    repetitions,
    statistic: input.statistic as string,
    rows: ordered,
    query_noise_floor_ns: input.query_noise_floor_ns as number,
    allocations_per_add: { flat, naive },
  }
}

// The single numeric display policy, shared byte-for-byte with
// scripts/sync_benchmarks.py. Timings and allocations round to DISPLAY_DECIMALS
// and then drop trailing zeros; ratios render at RATIO_DECIMALS with no trimming.
// DISPLAY_DECIMALS is 3 because that is the smallest precision that reproduces
// every accepted artifact string exactly (the naive allocation figure 2.064 has
// three fractional digits); a smaller cap would silently rewrite a published
// number.
export const DISPLAY_DECIMALS = 3
export const RATIO_DECIMALS = 1

// Expand a finite non-negative number's shortest round-trip decimal into a plain
// fixed-point [intPart, fracPart] with no exponent. Number.prototype.toString gives
// exponential form for very small (< 1e-6) or very large (>= 1e21) magnitudes, so
// this shifts the decimal point by the exponent using string manipulation only,
// never multiplying the binary float. Mirrors Python's Decimal(str(value)): a tiny
// value keeps its leading zeros and a huge one becomes a long integer.
function decimalParts(value: number): [string, string] {
  const text = value.toString()
  const match = /^(\d+)(?:\.(\d+))?(?:[eE]([+-]?\d+))?$/.exec(text)
  if (match === null) {
    throw new BaselineError(`cannot parse the decimal form of ${value}`)
  }
  const lead = match[1]!
  const digits = lead + (match[2] ?? '')
  const exponent = match[3] ? parseInt(match[3], 10) : 0
  // The decimal point currently sits after lead.length digits; the exponent moves
  // it right (positive) or left (negative).
  const pointPos = lead.length + exponent
  let intPart: string
  let fracPart: string
  if (pointPos <= 0) {
    intPart = '0'
    fracPart = '0'.repeat(-pointPos) + digits
  } else if (pointPos >= digits.length) {
    intPart = digits + '0'.repeat(pointPos - digits.length)
    fracPart = ''
  } else {
    intPart = digits.slice(0, pointPos)
    fracPart = digits.slice(pointPos)
  }
  return [intPart.replace(/^0+(?=\d)/, ''), fracPart]
}

// Round a finite non-negative number to `places` decimals with decimal
// round-half-up, taken from the value's shortest decimal representation
// (Number.prototype.toString) rather than the binary float. Mirrors
// scripts/sync_benchmarks.py._round_half_up so 30.555 renders "30.555", a ratio of
// exactly 1.25 renders "1.3", and an exponent-form value such as 1e-7 renders as
// fixed-point in both renderers. Returns a fixed-point string with exactly `places`
// fractional digits.
export function roundHalfUpDecimal(value: number, places: number): string {
  if (!Number.isFinite(value)) {
    throw new BaselineError(`cannot format non-finite value ${value}`)
  }
  if (value < 0) {
    throw new BaselineError(`cannot format negative value ${value}`)
  }
  let [intPart, fracPart] = decimalParts(value)
  if (fracPart.length > places) {
    // Round-half-up needs only the first dropped digit: a leading 5 (with or
    // without further digits) rounds up, anything less rounds down.
    const roundUp = fracPart.charCodeAt(places) - 48 >= 5
    fracPart = fracPart.slice(0, places)
    if (roundUp) {
      const digits = (intPart + fracPart).split('')
      let i = digits.length - 1
      for (; i >= 0; i--) {
        if (digits[i] === '9') {
          digits[i] = '0'
        } else {
          digits[i] = String(digits[i]!.charCodeAt(0) - 48 + 1)
          break
        }
      }
      if (i < 0) {
        digits.unshift('1')
      }
      const joined = digits.join('')
      const cut = joined.length - places
      intPart = joined.slice(0, cut)
      fracPart = joined.slice(cut)
    }
  } else {
    fracPart = fracPart.padEnd(places, '0')
  }
  return places === 0 ? intPart : `${intPart}.${fracPart}`
}

// A timing or allocation figure: decimal round-half-up to DISPLAY_DECIMALS, then
// trailing zeros and any bare decimal point trimmed, so 30.5 and 0.26 both read
// naturally and match the figures scripts/sync_benchmarks.py renders.
export function formatNs(value: number): string {
  const rounded = roundHalfUpDecimal(value, DISPLAY_DECIMALS)
  if (!rounded.includes('.')) {
    return rounded
  }
  return rounded.replace(/\.?0+$/, '')
}

// The ratio a "faster" row shows, at RATIO_DECIMALS with the shared round-half-up
// policy. Only meaningful when naive > flat, which the validator guarantees for
// faster rows.
export function formatRatio(row: BaselineRow): string {
  return `${roundHalfUpDecimal(row.naive_ns / row.flat_ns, RATIO_DECIMALS)}x faster`
}

// INTERPRETATIONS is exported for tests that enumerate the accepted set.
export { INTERPRETATIONS }
