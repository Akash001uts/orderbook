import type { BaselineArtifact, BaselineRow, LatencyArtifact } from '../data'
import { formatNs, formatRatio } from '../baseline'
import { BASELINE_CITATION, HERO_FOOTNOTES, PROJECT, ARCHITECTURE } from '../content'
import { decimal, element, integer, nanoseconds, scrollBox } from '../format'

// The hero. Headline numbers, and the caveats next to them rather than below the
// fold, because on this project the honesty is the differentiator.
//
// Every figure in the std::map comparison is derived from the numeric fields of
// baseline.json rather than stated, so the site cannot drift from the artifact the
// document also renders from. The formatting helpers live in ../baseline alongside
// the validator, so the site and scripts/sync_benchmarks.py print the same values.

function citation(document: string, section: string): HTMLElement {
  return element('p', { class: 'citation' }, `${document}, "${section}"`)
}

// The ns figures and the ratio are formatted from the numbers; the wording for a
// result below the harness floor comes from the row's interpretation, never from a
// ratio of two figures that small.
function baselineCells(row: BaselineRow): { flat: string; naive: string; ratio: string } {
  const flat = `${formatNs(row.flat_ns)} ns`
  const naive = `${formatNs(row.naive_ns)} ns`
  if (row.interpretation === 'below_resolution') {
    return { flat, naive, ratio: 'below measurement resolution' }
  }
  return { flat, naive, ratio: formatRatio(row) }
}

function baselineTable(baseline: BaselineArtifact): HTMLElement {
  const body = element('tbody')
  for (const row of baseline.rows) {
    const cells = baselineCells(row)
    body.append(
      element(
        'tr',
        {},
        element('th', { scope: 'row' }, row.label),
        element('td', { class: 'num' }, cells.flat),
        element('td', { class: 'num' }, cells.naive),
        element(
          'td',
          { class: `num ${row.interpretation === 'below_resolution' ? '' : 'good'}` },
          cells.ratio,
        ),
      ),
    )
  }

  return element(
    'table',
    { class: 'grid' },
    element(
      'thead',
      {},
      element(
        'tr',
        {},
        element('th', { scope: 'col' }, 'Operation'),
        element('th', { scope: 'col', class: 'num' }, 'Flat book'),
        element('th', { scope: 'col', class: 'num' }, 'std::map'),
        element('th', { scope: 'col', class: 'num' }, 'Ratio'),
      ),
    ),
    body,
  )
}

function liveFigures(latency: LatencyArtifact): HTMLElement {
  const all = latency.latency_ns.find((row) => row.bucket === 'ALL')
  const throughput = latency.throughput.mean_messages_per_second / 1e6

  const cards = [
    all && { value: nanoseconds(all.p50), label: 'p50, whole feed, per message' },
    all && { value: nanoseconds(all.p99), label: 'p99, whole feed' },
    all && { value: nanoseconds(all.p99_9), label: 'p99.9, whole feed' },
    { value: `${decimal(throughput, 1)} M/s`, label: 'messages replayed per second' },
    { value: integer(PROJECT.tests), label: 'tests' },
    { value: integer(PROJECT.ciJobs), label: 'CI jobs' },
  ].filter(Boolean) as Array<{ value: string; label: string }>

  return element(
    'div',
    { class: 'cards' },
    ...cards.map((card) =>
      element(
        'div',
        { class: 'card' },
        element('span', { class: 'card-value' }, card.value),
        element('span', { class: 'card-label' }, card.label),
      ),
    ),
  )
}

// Derived from the artifact so the figure cannot drift from the published one.
function allocationFootnote(baseline: BaselineArtifact): string {
  const flat = decimal(baseline.allocations_per_add.flat, 0)
  const naive = decimal(baseline.allocations_per_add.naive, 3)
  return (
    `Allocations per add are ${flat} against ${naive}, counted exactly rather than sampled. ` +
    'The std::map book allocates a list node and a hash node on every add, plus one map node per newly occupied level; the flat book allocates nothing after construction.'
  )
}

export function heroPanel(latency: LatencyArtifact, baseline: BaselineArtifact): HTMLElement {
  const all = latency.latency_ns.find((row) => row.bucket === 'ALL')
  const footnotes = [allocationFootnote(baseline), ...HERO_FOOTNOTES]

  return element(
    'header',
    { class: 'hero', id: 'top' },
    element('p', { class: 'eyebrow' }, 'C++20 limit order book and matching engine'),
    element('h1', {}, 'A flat, direct indexed order book, measured against a naive one'),
    element(
      'p',
      { class: 'lede' },
      'Almost every number on this page is read from a JSON artifact committed next to the code that produced it. ' +
        'The set for QQQ is regenerated and diffed byte for byte on every push, so the site cannot drift from the engine. ' +
        'The few figures that are stated rather than read, like the arena sweep further down, cite the document they come from, ' +
        'and the parts that cannot be reproduced by a runner say so and carry the hash of the capture they came from.',
    ),

    liveFigures(latency),

    element(
      'div',
      { class: 'hero-tables' },
      element(
        'div',
        {},
        element('h3', {}, 'Against a std::map book, same command stream, one process'),
        scrollBox(baselineTable(baseline)),
        citation(BASELINE_CITATION.document, BASELINE_CITATION.section),
      ),
      element(
        'div',
        {},
        element('h3', {}, 'Where these figures came from'),
        element(
          'table',
          { class: 'stats' },
          element(
            'tbody',
            {},
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Machine'),
              element('td', {}, latency.environment.cpu_brand),
            ),
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Compiler'),
              element('td', {}, `${latency.environment.compiler}, ${latency.environment.build_type}`),
            ),
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Clock'),
              element(
                'td',
                {},
                `${latency.clock.source}, ${decimal(latency.clock.ticks_per_ns, 5)} ticks/ns`,
              ),
            ),
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Timer overhead'),
              element(
                'td',
                {},
                `${decimal(latency.clock.overhead_ns, 2)} ns, included in every figure and not subtracted`,
              ),
            ),
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Core pinning'),
              element('td', {}, latency.pinning.detail),
            ),
            element(
              'tr',
              {},
              element('th', { scope: 'row' }, 'Frequency scaling'),
              element('td', {}, latency.frequency_scaling.detail),
            ),
            all
              ? element(
                  'tr',
                  {},
                  element('th', { scope: 'row' }, 'Max, all messages'),
                  element(
                    'td',
                    { class: 'bad' },
                    `${nanoseconds(all.max)}, a scheduling event rather than a property of the book`,
                  ),
                )
              : null,
          ),
        ),
        element(
          'p',
          { class: 'citation' },
          `Read live from bench/latency.json. This run reported ${latency.degraded_conditions} degraded condition${
            latency.degraded_conditions === 1 ? '' : 's'
          }.`,
        ),
      ),
    ),

    element(
      'details',
      { class: 'footnotes' },
      element('summary', {}, 'Four caveats that belong with those numbers'),
      element(
        'ul',
        {},
        ...footnotes.map((footnote) => element('li', {}, footnote)),
      ),
    ),

    element(
      'details',
      { class: 'footnotes' },
      element('summary', {}, 'The shape of the thing'),
      element('pre', { class: 'ascii' }, ARCHITECTURE.trim()),
    ),

    element(
      'p',
      { class: 'hero-links' },
      element('a', { href: '#symbols' }, 'Per symbol results'),
      element('span', { class: 'sep' }, '/'),
      element('a', { href: '#correctness' }, 'Correctness'),
      element('span', { class: 'sep' }, '/'),
      element('a', { href: '#measurement' }, 'Measurement'),
    ),
  )
}
