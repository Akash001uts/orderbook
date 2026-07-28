import type { IndexArtifact } from '../data'
import { ARENA_CURVE, ARENA_CITATION } from '../content'
import { chart, SERIES_COLOURS } from '../charts'
import { decimal, element, integer, percent } from '../format'

// The measurement story.
//
// The point of this section is not the arena default. It is the sequence: the
// default was derived from one symbol, checked against five, and moved back. That
// sequence is worth more than the number, and the sizing table is built live from
// index.json so it now rests on nine symbols rather than on a paragraph.

function arenaCurve(): HTMLElement {
  const capacities = ARENA_CURVE.map((row) => row.capacity)
  const quiet = ARENA_CURVE.map((row) => row.quiet)
  const loaded = ARENA_CURVE.map((row) => row.loaded)

  return chart({
    title: 'Cost of an add against arena capacity, the same sweep run twice',
    data: [capacities, quiet, loaded] as unknown as [number[], number[], number[]],
    series: [
      { label: 'quiet machine', colour: SERIES_COLOURS[3]! },
      { label: 'loaded machine', colour: SERIES_COLOURS[4]! },
    ],
    xLabel: 'arena capacity, orders',
    yLabel: 'nanoseconds per add',
    logX: true,
    height: 280,
    xValues: (_self, ticks) =>
      ticks.map((value) => (value >= 1024 ? `${Math.round(value / 1024)}k` : `${value}`)),
  })
}

function sizingTable(index: IndexArtifact): HTMLElement {
  const sorted = [...index.symbols].sort((a, b) => a.peak_live_orders - b.peak_live_orders)
  const deepest = sorted[sorted.length - 1]
  const shallowest = sorted[0]

  const body = element('tbody')
  for (const entry of sorted) {
    const headroomAtDefault = entry.library_arena_default / entry.peak_live_orders
    const headroomAtOld = 16384 / entry.peak_live_orders
    const wouldFail = entry.peak_live_orders > 16384

    body.append(
      element(
        'tr',
        { class: wouldFail ? 'highlight' : '' },
        element('th', { scope: 'row' }, entry.symbol),
        element('td', { class: 'num' }, integer(entry.messages_applied)),
        element('td', { class: 'num' }, integer(entry.peak_live_orders)),
        element(
          'td',
          { class: `num ${wouldFail ? 'bad' : ''}` },
          wouldFail
            ? `exceeds it by ${percent((entry.peak_live_orders / 16384 - 1) * 100, 0)}`
            : `${decimal(headroomAtOld, 1)}x`,
        ),
        element('td', { class: 'num good' }, `${decimal(headroomAtDefault, 1)}x`),
      ),
    )
  }

  const table = element(
    'table',
    { class: 'grid' },
    element(
      'thead',
      {},
      element(
        'tr',
        {},
        element('th', { scope: 'col' }, 'Symbol'),
        element('th', { scope: 'col', class: 'num' }, 'Messages'),
        element('th', { scope: 'col', class: 'num' }, 'Peak live orders'),
        element('th', { scope: 'col', class: 'num' }, 'Headroom at 16 384'),
        element('th', { scope: 'col', class: 'num' }, 'Headroom at 65 536'),
      ),
    ),
    body,
  )

  const spread =
    deepest && shallowest && shallowest.peak_live_orders > 0
      ? decimal(deepest.peak_live_orders / shallowest.peak_live_orders, 0)
      : 'many'

  return element(
    'div',
    {},
    element('div', { class: 'scroll-x' }, table),
    element(
      'p',
      { class: 'note' },
      `Peak depth varies by a factor of ${spread} across ${integer(
        index.symbols.length,
      )} mainstream symbols on one ordinary session. A default derived from any single one of them is a default fitted to that symbol.`,
    ),
  )
}

export function measurementPanel(index: IndexArtifact): HTMLElement {
  return element(
    'section',
    { class: 'panel', id: 'measurement' },
    element('h2', {}, 'Measurement'),

    element(
      'p',
      { class: 'lede' },
      'The band width and the arena capacity started as plausible round numbers defended afterwards. ' +
        'For a project whose whole argument is measurement that is a weak position, so both were re-derived from a real full day capture. ' +
        'The interesting part is not where they ended up. It is that the first derivation was wrong and the evidence said so.',
    ),

    element('h3', {}, 'The arena default was derived from one symbol, checked against five, and moved back'),
    element(
      'p',
      {},
      'The first derivation used QQQ alone and produced 16 384, at 1.85x headroom over that symbol’s 8 842 peak. ' +
        'Widening the evidence to five of the busiest names on the same session showed that was wrong: AAPL peaks at 27 097, which exceeds 16 384 by 65 percent. ' +
        'An exhausted arena truncates the book being reconstructed, which corrupts every number downstream of it, so the default went back to 65 536. ' +
        'It is the value this started with, but not what it started with: it is now the smallest power of two covering a measured worst case, with the sample published.',
    ),
    sizingTable(index),
    element(
      'p',
      { class: 'note' },
      'This table is built from the per symbol artifacts on this page rather than typed out, so it now rests on nine symbols rather than the five the original revision used.',
    ),

    element('h3', {}, 'The cost of that choice, and why the curve is published twice'),
    arenaCurve(),
    element(
      'p',
      {},
      'The sweep measures every point back to back in one process, so within a run the points are directly comparable. ' +
        'It was run twice, in two different machine states, and publishing only one would misrepresent the result. ' +
        'The ordering is identical in both and a smaller arena is never worse, so the direction of the decision is sound. ' +
        'The magnitude is not robust at the near end: the old default against the new one is 5 percent on the quiet run and 35 percent on the loaded one. ' +
        'That is not measurement error, it is the effect being measured. Arena size is a cache pressure effect, and how much cache pressure costs depends on what else is competing for the cache.',
    ),
    element(
      'p',
      { class: 'note' },
      'An earlier version of this claimed a flat 13 percent for the bucket boundary and 16 percent for the arena change. Both came from the loaded run alone. They are real numbers from a valid paired measurement, and quoting either as the figure would still have been wrong, because a second run of the same sweep disagrees by a factor of four on the same quantity.',
    ),
    element('p', { class: 'citation' }, `${ARENA_CITATION.document}, "${ARENA_CITATION.section}"`),

    element('h3', {}, 'What is deliberately not claimed'),
    element(
      'ul',
      { class: 'caveats' },
      element(
        'li',
        {},
        'No isolated core. Pinning is implemented and verified on Windows and Linux, but pinning is not isolation, so every maximum here is a scheduling event rather than a property of the book.',
      ),
      element(
        'li',
        {},
        'Latency and throughput are never measured in the same pass. Two timestamp reads and two fences cost about as much as a message does, so the instrumented loop understates throughput by 3.5 times. That is the easiest wrong number in the project to publish by accident, because it falls out of the same run as the percentiles.',
      ),
      element(
        'li',
        {},
        'No hardware performance counters. The PMU is genuinely absent under WSL2, confirmed rather than inferred, so allocation counts, a working set sweep run on both implementations, and a structural pointer hop count stand in for perf stat.',
      ),
      element(
        'li',
        {},
        'Single symbol, single thread. The throughput figure is not a statement about a multi-symbol engine and no attempt is made to extrapolate it into one.',
      ),
    ),
  )
}
