import type { BacktestArtifact, SweepsArtifact } from '../data'
import { bar, chart, SERIES_COLOURS } from '../charts'
import {
  decimal,
  duration,
  element,
  integer,
  percent,
  signedDecimal,
  statTable,
} from '../format'

// The backtest panel.
//
// Two readings this panel exists to get right, both of which a chart drawn
// carelessly would get wrong.
//
// The markout curve is signed P&L per share against mid at each horizon. Where it
// crosses zero is the interesting part: fills that look healthy at one second can
// be underwater at five minutes, and that sign flip is adverse selection arriving
// late. The zero line is drawn and the crossing is named in prose.
//
// In the latency sweep, fills rise as latency grows and that is not latency
// helping. The cancel is delayed as well as the placement, so a quote the strategy
// has already decided to pull stays resting and fillable for the whole window. The
// 1 s markout is what those extra fills are worth, and it falls.

function attribution(backtest: BacktestArtifact): HTMLElement {
  const { pnl } = backtest
  const parts: Array<[string, number, string]> = [
    ['Spread capture', pnl.spread, 'pos'],
    ['Inventory', pnl.inventory, pnl.inventory >= 0 ? 'pos' : 'neg'],
    ['Fees and rebates', pnl.fees, pnl.fees >= 0 ? 'pos' : 'neg'],
    ['Total', pnl.total, pnl.total >= 0 ? 'total pos' : 'total neg'],
  ]
  const largest = Math.max(...parts.map(([, value]) => Math.abs(value)), 1)

  return element(
    'div',
    { class: 'waterfall' },
    ...parts.map(([label, value, className]) =>
      bar(label, signedDecimal(value, 2), Math.abs(value) / largest, className),
    ),
    element('p', { class: 'note' }, `Units are ${pnl.unit}.`),
  )
}

function markoutChart(backtest: BacktestArtifact): HTMLElement {
  const horizons = backtest.markouts.map((row) => row.horizon_ns / 1e9)
  const buys = backtest.markouts.map((row) => row.buy_per_share)
  const sells = backtest.markouts.map((row) => row.sell_per_share)
  const all = backtest.markouts.map((row) => row.all_per_share)

  return chart({
    title: 'Markouts, signed P&L per share against mid',
    data: [horizons, buys, sells, all] as unknown as [number[], ...Array<Array<number | null>>],
    series: [
      { label: 'buy', colour: SERIES_COLOURS[0]! },
      { label: 'sell', colour: SERIES_COLOURS[1]! },
      { label: 'all', colour: SERIES_COLOURS[3]! },
    ],
    xLabel: 'horizon, seconds',
    yLabel: 'per share',
    logX: true,
    zeroLine: true,
    xValues: (_self, ticks) =>
      ticks.map((value) => (value < 1 ? `${Math.round(value * 1000)}ms` : `${Math.round(value)}s`)),
  })
}

/** Names where the curve crosses zero, because that is the finding and a reader
 *  should not have to squint at a line to get it. */
function markoutReading(backtest: BacktestArtifact): HTMLElement {
  const points = backtest.markouts.filter((row) => row.all_per_share !== null)

  // The direction of a sign flip decides what it means, so the sentence is built
  // from the data rather than picked in advance. A curve that starts positive and
  // ends negative is adverse selection arriving late. One that starts negative and
  // recovers is the opposite finding, and describing it with the same sentence
  // would be worse than saying nothing.
  const flips: Array<{ from: string; to: string; negative: boolean }> = []
  for (let index = 1; index < points.length; index += 1) {
    const before = points[index - 1]!
    const after = points[index]!
    const a = before.all_per_share!
    const b = after.all_per_share!
    if ((a >= 0 && b < 0) || (a < 0 && b >= 0)) {
      flips.push({ from: before.horizon, to: after.horizon, negative: b < 0 })
    }
  }

  const first = points[0]
  const last = points[points.length - 1]
  const endsNegative = last !== undefined && last.all_per_share! < 0
  const startsNegative = first !== undefined && first.all_per_share! < 0

  let summary: string
  if (flips.length === 0) {
    summary = endsNegative
      ? 'The curve is negative at every horizon, which says the fills are adversely selected no matter how long you wait.'
      : 'The curve is positive at every measured horizon, so on this day and this symbol the fills were not being picked off.'
  } else if (flips.length === 1 && flips[0]!.negative) {
    summary = `The curve turns negative between ${flips[0]!.from} and ${flips[0]!.to}. That is adverse selection arriving late: each fill looked fine when it happened, and the position built out of them did not.`
  } else if (flips.length === 1 && startsNegative) {
    summary = `The curve is negative at short horizons and turns positive between ${flips[0]!.from} and ${flips[0]!.to}. The immediate move is against the fills and it reverses, which is the opposite of the usual finding and worth reading twice before believing.`
  } else {
    summary = `The curve crosses zero ${flips.length} times, ${flips
      .map((flip) => `${flip.negative ? 'down' : 'up'} between ${flip.from} and ${flip.to}`)
      .join(', ')}. Crossings in both directions across ${integer(
      last?.fills ?? 0,
    )} fills mean this is noise about zero rather than a signal, and it should not be read as either.`
  }

  const unresolved =
    backtest.unresolved_markouts > 0
      ? ` ${integer(backtest.unresolved_markouts)} fills were still unresolved when the replay ended; a horizon whose fills are mostly unresolved has not been measured.`
      : ' Every fill resolved at every horizon before the replay ended.'

  const holding = ` Read these against the holding time of ${duration(
    backtest.risk.time_holding_ns,
  )}. The longest horizon here is 300 s, so healthy markouts say each fill was individually fine and say nothing about the position built out of them, which is where the money actually went.`

  return element('p', { class: 'note' }, summary + unresolved + holding)
}

function latencySweepChart(sweeps: SweepsArtifact): HTMLElement | null {
  const rows = sweeps.latency_sweep
  if (!rows || rows.length === 0) return null

  // Zero is a real setting on this axis and a log scale cannot show it, so the
  // x axis is the row index and the labels carry the actual microseconds.
  const indices = rows.map((_row, index) => index)
  const fills = rows.map((row) => row.fills)
  const markouts = rows.map((row) => row.markout_1s_per_share)

  return element(
    'div',
    { class: 'chart-pair' },
    chart({
      title: 'Fills against reaction latency',
      data: [indices, fills] as unknown as [number[], number[]],
      series: [{ label: 'fills', colour: SERIES_COLOURS[2]! }],
      xLabel: 'reaction latency',
      yLabel: 'fills',
      height: 220,
      xValues: (_self, ticks) =>
        ticks.map((value) => {
          const row = rows[Math.round(value)]
          return row ? `${row.latency_us}us` : ''
        }),
    }),
    chart({
      title: '1 s markout against reaction latency',
      data: [indices, markouts] as unknown as [number[], Array<number | null>],
      series: [{ label: '1 s markout per share', colour: SERIES_COLOURS[4]! }],
      xLabel: 'reaction latency',
      yLabel: 'per share',
      height: 220,
      zeroLine: true,
      xValues: (_self, ticks) =>
        ticks.map((value) => {
          const row = rows[Math.round(value)]
          return row ? `${row.latency_us}us` : ''
        }),
    }),
  )
}

function parameterSweepTable(sweeps: SweepsArtifact): HTMLElement | null {
  const rows = sweeps.parameter_sweep
  if (!rows || rows.length === 0) return null

  const body = element('tbody')
  for (const row of rows) {
    body.append(
      element(
        'tr',
        {},
        element('td', { class: 'num' }, integer(row.offset_ticks)),
        element('td', { class: 'num' }, decimal(row.skew_ticks, 1)),
        element('td', { class: 'num' }, integer(row.size)),
        element(
          'td',
          { class: `num ${row.total_pnl >= 0 ? 'good' : 'bad'}` },
          signedDecimal(row.total_pnl, 0),
        ),
        element('td', { class: 'num' }, integer(row.fills)),
        element('td', { class: 'num' }, percent(row.fill_ratio * 100, 2)),
        element('td', { class: 'num' }, decimal(row.max_drawdown, 0)),
        element(
          'td',
          { class: `num ${(row.markout_1s_per_share ?? 0) >= 0 ? 'good' : 'bad'}` },
          row.markout_1s_per_share === null ? 'none' : signedDecimal(row.markout_1s_per_share, 4),
        ),
      ),
    )
  }

  return element(
    'details',
    { class: 'sweep' },
    element('summary', {}, `Parameter sensitivity grid, ${rows.length} cells`),
    element(
      'p',
      { class: 'note' },
      'Sensitivity analysis, not a search for an edge. This grid covers one symbol on one day, so the best cell is a property of that day and nothing more. ' +
        'What is worth reading is the shape: whether P&L degrades smoothly as the quote widens, whether skew reduces drawdown, and whether the markout stays negative everywhere.',
    ),
    element(
      'div',
      { class: 'scroll-x' },
      element(
        'table',
        { class: 'grid' },
        element(
          'thead',
          {},
          element(
            'tr',
            {},
            element('th', { scope: 'col', class: 'num' }, 'Offset'),
            element('th', { scope: 'col', class: 'num' }, 'Skew'),
            element('th', { scope: 'col', class: 'num' }, 'Size'),
            element('th', { scope: 'col', class: 'num' }, 'Total P&L'),
            element('th', { scope: 'col', class: 'num' }, 'Fills'),
            element('th', { scope: 'col', class: 'num' }, 'Fill ratio'),
            element('th', { scope: 'col', class: 'num' }, 'Drawdown'),
            element('th', { scope: 'col', class: 'num' }, '1 s markout'),
          ),
        ),
        body,
      ),
    ),
  )
}

export function backtestPanel(backtest: BacktestArtifact, sweeps: SweepsArtifact): HTMLElement {
  const { activity, crossed_book: crossed, config, sharpe } = backtest

  return element(
    'div',
    { class: 'panel-body' },
    element(
      'p',
      { class: 'note' },
      `Quoting ${integer(config.quote_size)} shares ${integer(
        config.quote_offset_ticks,
      )} tick from mid, position limit ${integer(config.position_limit)}, inventory skew ${decimal(
        config.inventory_skew_ticks,
        1,
      )} ticks, reaction latency ${integer(config.latency_us)} us. ` +
        (config.latency_us === 0
          ? 'Zero latency is an idealised bound rather than a realistic setting for any real participant.'
          : ''),
    ),

    element(
      'div',
      { class: 'columns' },
      element(
        'div',
        { class: 'column' },
        element('h3', {}, 'P&L attribution'),
        attribution(backtest),
        statTable([
          ['Final position', `${integer(backtest.risk.final_position)} shares`],
          [
            'Max long / short',
            `${integer(backtest.risk.max_long)} / ${integer(backtest.risk.max_short)}`,
          ],
          ['Max drawdown', decimal(backtest.risk.max_drawdown, 2)],
          ['Time holding', duration(backtest.risk.time_holding_ns)],
          [
            'Sharpe',
            sharpe.value === null
              ? 'not computable on this sample'
              : `${decimal(sharpe.value, 3)} over ${integer(sharpe.samples)} one second samples, ${integer(
                  sharpe.informative_samples,
                )} non-zero, not annualised${
                  sharpe.clears_minimum_events
                    ? ''
                    : '. Fewer than 30 fills, so a mean and a standard deviation do not describe anything here'
                }`,
          ],
        ]),
        element(
          'p',
          { class: 'note' },
          'Treat the Sharpe as descriptive regardless. The series is dominated by mark to market on a position held for a long time, so the samples are strongly autocorrelated, which understates the variance and therefore overstates the ratio.',
        ),
      ),
      element(
        'div',
        { class: 'column' },
        element('h3', {}, 'Activity and the honesty checks'),
        statTable([
          [
            'Fills',
            `${integer(activity.fills)} for ${integer(activity.filled_shares)} shares, ${percent(
              activity.fill_ratio * 100,
              2,
            )} of ${integer(activity.quoted_shares)} quoted`,
          ],
          [
            'Of which trade through',
            `${integer(activity.trade_through_fills)}, inferred from the market trading past our price`,
          ],
          [
            'Quotes',
            `${integer(activity.quotes_placed)} placed, ${integer(
              activity.quotes_cancelled,
            )} cancelled, ${integer(activity.quotes_rejected)} post-only rejected`,
          ],
          [
            'Cancels too late',
            `${integer(activity.cancels_too_late)}, the quote had already filled when the cancel landed`,
          ],
          [
            'Limit overshoot',
            `${integer(
              activity.limit_overshoot_shares,
            )} shares beyond the position limit, from quotes in flight when it was reached`,
          ],
          [
            'Crossed book',
            `${integer(crossed.intervals)} intervals over ${duration(crossed.total_ns)}, ${percent(
              crossed.crossed_percent,
              4,
            )} of ${integer(crossed.observations)} book updates, worst ${integer(
              crossed.worst_depth_ticks,
            )} ticks deep`,
          ],
          [
            'Locked, not crossed',
            `${integer(crossed.locked_observations)} observations with bid equal to ask, counted separately`,
          ],
        ]),
        element(
          'p',
          { class: 'note' },
          'A crossed interval is time the quote sat inside the real spread with nobody trading against it. Those are not fills, and counting them as fills is exactly where an optimistic backtest would hide.',
        ),
      ),
    ),

    element('h3', {}, 'Markouts'),
    markoutChart(backtest),
    markoutReading(backtest),

    element('h3', {}, 'What latency costs'),
    latencySweepChart(sweeps) ?? element('p', { class: 'note' }, 'No latency sweep in this artifact.'),
    element(
      'p',
      { class: 'note' },
      'Read the two together, because separately they say opposite things. Fills rise as latency grows, which looks like latency helping and is not: the cancel is delayed as well as the placement, so a quote the strategy has already decided to pull stays resting and fillable for the whole window. More exposure, more fills. The 1 s markout is what those extra fills are worth, and it falls. That is adverse selection appearing exactly where theory says it should.',
    ),

    parameterSweepTable(sweeps) ?? element('span', {}),
  )
}
