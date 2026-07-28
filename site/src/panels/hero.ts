import type { LatencyArtifact } from '../data'
import { BASELINE, BASELINE_CITATION, HERO_FOOTNOTES, PROJECT, ARCHITECTURE } from '../content'
import { decimal, element, integer, nanoseconds, scrollBox } from '../format'

// The hero. Headline numbers, and the caveats next to them rather than below the
// fold, because on this project the honesty is the differentiator.

function citation(document: string, section: string): HTMLElement {
  return element('p', { class: 'citation' }, `${document}, "${section}"`)
}

function baselineTable(): HTMLElement {
  const body = element('tbody')
  for (const row of BASELINE) {
    body.append(
      element(
        'tr',
        {},
        element('th', { scope: 'row' }, row.operation),
        element('td', { class: 'num' }, row.flat),
        element('td', { class: 'num' }, row.naive),
        element('td', { class: `num ${row.faster ? 'good' : 'bad'}` }, row.ratio),
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

export function heroPanel(latency: LatencyArtifact): HTMLElement {
  const all = latency.latency_ns.find((row) => row.bucket === 'ALL')

  return element(
    'header',
    { class: 'hero', id: 'top' },
    element('p', { class: 'eyebrow' }, 'C++20 limit order book and matching engine'),
    element('h1', {}, 'A flat, direct indexed order book, measured against a naive one'),
    element(
      'p',
      { class: 'lede' },
      'Every number on this page is read from a JSON artifact committed next to the code that produced it. ' +
        'The set for QQQ is regenerated and diffed byte for byte on every push, so the site cannot drift from the engine. ' +
        'The parts that cannot be reproduced by a runner say so, and carry the hash of the capture they came from.',
    ),

    liveFigures(latency),

    element(
      'div',
      { class: 'hero-tables' },
      element(
        'div',
        {},
        element('h3', {}, 'Against a std::map book, same command stream, one process'),
        scrollBox(baselineTable()),
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
        ...HERO_FOOTNOTES.map((footnote) => element('li', {}, footnote)),
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
