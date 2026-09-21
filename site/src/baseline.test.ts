// Runtime tests for the baseline validator and formatters.
//
// Dependency free on purpose: run with Node's type stripping so CI needs no test
// runner and no extra install step:
//
//     node --experimental-strip-types src/baseline.test.ts
//
// The cases mirror scripts/test_sync_benchmarks.py so the TypeScript and Python
// validators agree on what a valid artifact is. The .ts import extension is what
// Node needs to resolve the module at runtime; allowImportingTsExtensions in
// tsconfig lets tsc accept it too.

import {
  BaselineError,
  formatNs,
  formatRatio,
  validateBaseline,
  type BaselineArtifact,
} from './baseline.ts'

let failures = 0

function fail(name: string, detail: string): void {
  failures += 1
  console.error(`FAIL ${name}: ${detail}`)
}

function good(): Record<string, unknown> {
  return {
    schema: 1,
    tool: 'ob_baseline_bench',
    version: '0.1.0',
    benchmark_command: './ob_baseline_bench',
    repetitions: 9,
    statistic: 'median',
    rows: [
      { id: 'add', label: 'Add', flat_ns: 30.5, naive_ns: 84.7, interpretation: 'faster' },
      { id: 'cancel', label: 'Cancel', flat_ns: 11.5, naive_ns: 61.1, interpretation: 'faster' },
      { id: 'match', label: 'Match', flat_ns: 32.2, naive_ns: 67.4, interpretation: 'faster' },
      {
        id: 'best_bid',
        label: 'Best bid',
        flat_ns: 0.26,
        naive_ns: 4.45,
        interpretation: 'below_resolution',
      },
    ],
    query_noise_floor_ns: 0.17,
    allocations_per_add: { flat: 0, naive: 2.064 },
  }
}

function expectOk(name: string, value: unknown): void {
  try {
    validateBaseline(value)
  } catch (error) {
    fail(name, `expected valid, threw ${String(error)}`)
  }
}

function expectError(name: string, mutate: (data: Record<string, unknown>) => void): void {
  const data = good()
  mutate(data)
  try {
    validateBaseline(data)
    fail(name, 'expected BaselineError, none thrown')
  } catch (error) {
    if (!(error instanceof BaselineError)) {
      fail(name, `threw the wrong error type ${String(error)}`)
    }
  }
}

function rows(data: Record<string, unknown>): Array<Record<string, unknown>> {
  return data.rows as Array<Record<string, unknown>>
}

function expectEqual(name: string, actual: string, expected: string): void {
  if (actual !== expected) {
    fail(name, `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`)
  }
}

// Valid input.
expectOk('baseline seed validates', good())

// The reproduced case: still marked faster but flat is not below naive.
expectError('faster row with flat above naive', (d) => {
  rows(d)[0]!.flat_ns = 100
})
expectError('faster row with flat equal to naive', (d) => {
  rows(d)[0]!.flat_ns = 84.7
  rows(d)[0]!.naive_ns = 84.7
})

// Allocations and noise floor.
expectError('negative flat allocations', (d) => {
  ;(d.allocations_per_add as Record<string, unknown>).flat = -1
})
expectError('negative naive allocations', (d) => {
  ;(d.allocations_per_add as Record<string, unknown>).naive = -0.5
})
expectError('missing noise floor', (d) => {
  delete d.query_noise_floor_ns
})
expectError('negative noise floor', (d) => {
  d.query_noise_floor_ns = -0.1
})

// Schema, rows, interpretations.
expectError('wrong schema', (d) => {
  d.schema = 2
})
expectError('missing required row', (d) => {
  rows(d).pop()
})
expectError('unknown interpretation', (d) => {
  rows(d)[0]!.interpretation = 'mystery'
})
expectError('non finite timing', (d) => {
  rows(d)[0]!.flat_ns = Number.POSITIVE_INFINITY
})
expectError('duplicate row id', (d) => {
  rows(d).push({ ...rows(d)[0]! })
})

// Shape and metadata: unknown input must be rejected.
expectError('missing tool', (d) => {
  delete d.tool
})
expectError('empty version', (d) => {
  d.version = ''
})
expectError('missing benchmark_command', (d) => {
  delete d.benchmark_command
})
expectError('non integer repetitions', (d) => {
  d.repetitions = 9.5
})
expectError('zero repetitions', (d) => {
  d.repetitions = 0
})
expectError('empty label', (d) => {
  rows(d)[0]!.label = ''
})
expectError('row not an object', (d) => {
  rows(d)[0] = 'add' as unknown as Record<string, unknown>
})

// Non-object top level, checked directly since expectError mutates a good() dict.
for (const value of [null, undefined, [], 'x', 3]) {
  try {
    validateBaseline(value)
    fail(`top level ${String(value)}`, 'expected BaselineError, none thrown')
  } catch (error) {
    if (!(error instanceof BaselineError)) {
      fail(`top level ${String(value)}`, `threw the wrong error type ${String(error)}`)
    }
  }
}

// Formatting must match the Python renderer's %g and %.1f rules.
const validated: BaselineArtifact = validateBaseline(good())
expectEqual('formatNs trims trailing zero', formatNs(30.5), '30.5')
expectEqual('formatNs keeps two decimals', formatNs(4.45), '4.45')
expectEqual('formatNs whole number', formatNs(84.7), '84.7')
expectEqual('formatRatio one decimal', formatRatio(validated.rows[0]!), '2.8x faster')

if (failures > 0) {
  console.error(`${failures} baseline validator check(s) failed`)
  throw new Error('baseline.test.ts failed')
}
console.log('all baseline.ts validator checks passed')
