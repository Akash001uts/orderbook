// Runtime tests for the baseline validator and formatters.
//
// Dependency free on purpose: run with Node's type stripping so CI needs no test
// runner and no extra install step:
//
//     node --experimental-strip-types src/baseline.test.ts
//
// Validation and formatting cases come from test/fixtures/benchmarks/, the same
// files scripts/test_sync_benchmarks.py consumes, so the TypeScript and Python
// validators cannot drift apart: a divergence fails one suite. The .ts import
// extension is what Node needs to resolve the module at runtime;
// allowImportingTsExtensions in tsconfig lets tsc accept it too.

import {
  BaselineError,
  formatNs,
  formatRatio,
  roundHalfUpDecimal,
  validateBaseline,
  RATIO_DECIMALS,
  type BaselineArtifact,
} from './baseline.ts'
import displayCasesRaw from '../../test/fixtures/benchmarks/display_cases.json' with { type: 'json' }
import validationRaw from '../../test/fixtures/benchmarks/validation_cases.json' with { type: 'json' }

// The fixtures are deliberately untyped here: they carry heterogeneous cases whose
// shapes are the thing under test, so a precise type would fight the test rather
// than help it.
const displayCases = displayCasesRaw as any
const validation = validationRaw as any

let failures = 0

function fail(name: string, detail: string): void {
  failures += 1
  console.error(`FAIL ${name}: ${detail}`)
}

function expectEqual(name: string, actual: string, expected: string): void {
  if (actual !== expected) {
    fail(name, `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`)
  }
}

// Apply a fixture case's declarative edits to the shared base. Mirrors
// apply_case in scripts/test_sync_benchmarks.py.
function applyCase(base: any, testCase: any): unknown {
  if ('replace' in testCase) {
    return structuredClone(testCase.replace)
  }
  const data = structuredClone(base)
  if (testCase.set) {
    Object.assign(data, testCase.set)
  }
  if (Array.isArray(testCase.delete)) {
    for (const key of testCase.delete) {
      delete data[key]
    }
  }
  if ('rows' in testCase) {
    data.rows = structuredClone(testCase.rows)
  }
  if ('alloc' in testCase) {
    data.allocations_per_add = structuredClone(testCase.alloc)
  }
  return data
}

// Display formatting parity.
for (const testCase of displayCases.format_ns) {
  expectEqual(`format_ns ${testCase.name}`, formatNs(testCase.value), testCase.expected)
}
for (const testCase of displayCases.format_ratio) {
  expectEqual(
    `format_ratio ${testCase.name}`,
    roundHalfUpDecimal(testCase.numerator / testCase.denominator, RATIO_DECIMALS),
    testCase.expected,
  )
}

// formatRatio wraps the same rounding with the "x faster" wording.
const seed: BaselineArtifact = validateBaseline(structuredClone(validation.base))
expectEqual('formatRatio add row', formatRatio(seed.rows[0]!), '2.8x faster')

// Validation parity.
for (const testCase of validation.cases) {
  const data = applyCase(validation.base, testCase)
  let accepted = true
  try {
    validateBaseline(data)
  } catch (error) {
    accepted = false
    if (!(error instanceof BaselineError)) {
      fail(testCase.name, `threw the wrong error type ${String(error)}`)
    }
  }
  if (accepted !== testCase.valid) {
    fail(testCase.name, `expected ${testCase.valid ? 'valid' : 'invalid'}, got the opposite`)
  }
}

// Non-finite values cannot be encoded in JSON, so they are checked in code.
function expectRejected(name: string, build: (base: any) => unknown): void {
  const data = build(structuredClone(validation.base))
  try {
    validateBaseline(data)
    fail(name, 'expected BaselineError, none thrown')
  } catch (error) {
    if (!(error instanceof BaselineError)) {
      fail(name, `threw the wrong error type ${String(error)}`)
    }
  }
}
expectRejected('infinite flat timing', (d) => {
  d.rows[0].flat_ns = Number.POSITIVE_INFINITY
  return d
})
expectRejected('nan flat timing', (d) => {
  d.rows[0].flat_ns = Number.NaN
  return d
})
expectRejected('infinite noise floor', (d) => {
  d.query_noise_floor_ns = Number.POSITIVE_INFINITY
  return d
})
expectRejected('nan naive allocation', (d) => {
  d.allocations_per_add.naive = Number.NaN
  return d
})

// Row-order normalization parity.
for (const testCase of validation.order_cases) {
  const data: any = structuredClone(validation.base)
  if ('rows' in testCase) {
    data.rows = structuredClone(testCase.rows)
  }
  const result = validateBaseline(data)
  const got = result.rows.map((row) => row.id).join(',')
  expectEqual(`order ${testCase.name}`, got, (testCase.expected_order as string[]).join(','))
}

if (failures > 0) {
  console.error(`${failures} baseline validator check(s) failed`)
  throw new Error('baseline.test.ts failed')
}
console.log('all baseline.ts validator checks passed')
