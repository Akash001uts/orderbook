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
  if (typeof value !== 'string' || value.length === 0) {
    throw new BaselineError(`baseline.json: ${field} must be a non-empty string`)
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
    if (interpretation === 'faster' && naive_ns <= flat_ns) {
      throw new BaselineError(`baseline.json: row ${id} is marked faster but flat is not below naive`)
    }
    validated.push({ id, label, flat_ns, naive_ns, interpretation })
  }

  for (const id of REQUIRED_BASELINE_ROWS) {
    if (!ids.has(id)) {
      throw new BaselineError(`baseline.json: missing required row id ${id}`)
    }
  }

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
    rows: validated,
    query_noise_floor_ns: input.query_noise_floor_ns as number,
    allocations_per_add: { flat, naive },
  }
}

// Two decimals, trailing zeros trimmed, so 30.5 and 0.26 both read naturally and
// match the figures scripts/sync_benchmarks.py renders from the same numbers.
export function formatNs(value: number): string {
  return value.toFixed(2).replace(/\.?0+$/, '')
}

// The ratio a "faster" row shows. Only meaningful when naive > flat, which the
// validator guarantees for faster rows.
export function formatRatio(row: BaselineRow): string {
  return `${(row.naive_ns / row.flat_ns).toFixed(1)}x faster`
}

// INTERPRETATIONS is exported for tests that enumerate the accepted set.
export { INTERPRETATIONS }
