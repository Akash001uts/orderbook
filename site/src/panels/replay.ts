import type { DepthArtifact, ReplayArtifact, SourceArtifact } from '../data'
import {
  bytes,
  decimal,
  element,
  integer,
  percent,
  price,
  scrollBox,
  shortHash,
  statTable,
} from '../format'

// The replay panel. What the engine saw in one symbol's tape, and what it did with
// it.
//
// The sizing block is the part worth reading twice. The arena default this project
// ships was derived from QQQ alone, checked against four more symbols, and moved
// back when AAPL turned out to need four times what QQQ does. The bar next to each
// symbol's peak is that argument, drawn.

function messageTable(replay: ReplayArtifact): HTMLElement {
  const rows: Array<[string, number, string]> = [
    ['Adds', replay.messages.adds, 'A and F'],
    ['Deletes', replay.messages.deletes, 'D'],
    ['Replaces', replay.messages.replaces, 'U'],
    ['Cancels', replay.messages.cancels, 'X'],
    ['Executions', replay.messages.executions, 'E and C'],
    ['Trades, not applied', replay.messages.trades, 'P'],
    ['Cross trades, not applied', replay.messages.cross_trades, 'Q'],
    ['Broken trades, not applied', replay.messages.broken_trades, 'B'],
    ['System events', replay.messages.system_events, 'S'],
  ]

  const total = replay.messages.applied || 1
  const body = element('tbody')
  for (const [label, count, code] of rows) {
    const share = count / total
    body.append(
      element(
        'tr',
        {},
        element('th', { scope: 'row' }, label),
        element('td', { class: 'code' }, code),
        element('td', { class: 'num' }, integer(count)),
        element(
          'td',
          { class: 'sparkbar' },
          element('span', { style: `width: ${Math.min(100, share * 100)}%` }),
        ),
        element('td', { class: 'num muted' }, percent(share * 100, 1)),
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
        element('th', { scope: 'col' }, 'Message'),
        element('th', { scope: 'col' }, 'Type'),
        element('th', { scope: 'col', class: 'num' }, 'Count'),
        element('th', { scope: 'col' }, ''),
        element('th', { scope: 'col', class: 'num' }, 'Share'),
      ),
    ),
    body,
  )
}

// Bids left, asks right, quantity as bar length, both scaled to the same maximum
// so the two sides can be compared rather than only read.
function ladder(depth: DepthArtifact): HTMLElement {
  const quantities = [...depth.bids, ...depth.asks].map((level) => level.quantity)
  const largest = Math.max(1, ...quantities)
  const rows = Math.max(depth.bids.length, depth.asks.length)

  const body = element('tbody')
  for (let index = 0; index < rows; index += 1) {
    const bid = depth.bids[index]
    const ask = depth.asks[index]

    body.append(
      element(
        'tr',
        {},
        element('td', { class: 'num muted' }, bid ? integer(bid.orders) : ''),
        element('td', { class: 'num' }, bid ? integer(bid.quantity) : ''),
        element(
          'td',
          { class: 'ladder-bar bid' },
          bid
            ? element('span', { style: `width: ${(bid.quantity / largest) * 100}%` })
            : element('span', { style: 'width: 0' }),
        ),
        element('td', { class: 'num bid-price' }, bid ? price(bid.price, depth.price_scale) : ''),
        element('td', { class: 'num ask-price' }, ask ? price(ask.price, depth.price_scale) : ''),
        element(
          'td',
          { class: 'ladder-bar ask' },
          ask
            ? element('span', { style: `width: ${(ask.quantity / largest) * 100}%` })
            : element('span', { style: 'width: 0' }),
        ),
        element('td', { class: 'num' }, ask ? integer(ask.quantity) : ''),
        element('td', { class: 'num muted' }, ask ? integer(ask.orders) : ''),
      ),
    )
  }

  return element(
    'table',
    { class: 'grid ladder' },
    element(
      'thead',
      {},
      element(
        'tr',
        {},
        element('th', { scope: 'col', class: 'num' }, 'Ord'),
        element('th', { scope: 'col', class: 'num' }, 'Qty'),
        element('th', { scope: 'col' }, 'Bids'),
        element('th', { scope: 'col', class: 'num' }, 'Bid'),
        element('th', { scope: 'col', class: 'num' }, 'Ask'),
        element('th', { scope: 'col' }, 'Asks'),
        element('th', { scope: 'col', class: 'num' }, 'Qty'),
        element('th', { scope: 'col', class: 'num' }, 'Ord'),
      ),
    ),
    body,
  )
}

function sizingBlock(replay: ReplayArtifact): HTMLElement {
  const { sizing } = replay
  const headroom = sizing.library_arena_default / sizing.peak_live_orders
  const used = sizing.peak_live_orders / sizing.library_arena_default
  const tight = used > 0.25

  return element(
    'div',
    { class: 'sizing' },
    element(
      'div',
      { class: 'sizing-bar' },
      element('span', { class: `sizing-fill ${tight ? 'tight' : ''}`, style: `width: ${Math.min(100, used * 100)}%` }),
      element(
        'span',
        { class: 'sizing-caption' },
        `${integer(sizing.peak_live_orders)} peak live orders in an arena of ${integer(
          sizing.library_arena_default,
        )}, ${decimal(headroom, 2)}x headroom`,
      ),
    ),
    statTable([
      ['Peak occupied levels', `${integer(sizing.peak_occupied_levels)} of ${integer(sizing.band_levels)} band levels`],
      [
        'Touch travelled',
        sizing.touch_span_ticks === null
          ? 'the book never had two sides'
          : `${integer(sizing.touch_span_ticks)} ticks, ${percent(
              sizing.touch_span_band_percent ?? 0,
              2,
            )} of the band`,
      ],
      ['Band rebases', `${integer(replay.book.rebases)}, ${integer(replay.book.rebases_abandoned)} abandoned`],
      [
        'Cold levels used',
        `${integer(replay.book.cold_levels)}, ${integer(replay.book.cold_operations)} operations against the out of band path`,
      ],
      [
        'Off tick prices',
        `${integer(replay.rejections.off_tick_prices)}, a statement about the configured tick rather than a defect`,
      ],
      [
        'Rejected adds',
        replay.rejections.rejected_adds === 0
          ? '0, so no liquidity was silently lost'
          : `${integer(replay.rejections.rejected_adds)}, which makes every number below suspect`,
      ],
      [
        'Unknown order references',
        `${integer(
          replay.rejections.unknown_order_references,
        )}, expected at the start of a file since the session began before the capture did`,
      ],
    ]),
  )
}

function provenanceBlock(replay: ReplayArtifact, source: SourceArtifact | null): HTMLElement {
  return element(
    'details',
    { class: 'provenance' },
    element(
      'summary',
      {},
      source && !source.reproducible_in_ci
        ? 'Provenance: derived from a capture no runner can fetch'
        : 'Provenance',
    ),
    statTable([
      ['Input', replay.provenance.source],
      ['Input size', bytes(replay.provenance.source_bytes)],
      ['Input SHA-256', shortHash(replay.provenance.source_sha256)],
      ['Command', element('code', {}, replay.provenance.command)],
      source ? ['Capture', source.capture] : null,
      source ? ['Capture size', bytes(source.capture_bytes)] : null,
      source ? ['Capture SHA-256', shortHash(source.capture_sha256)] : null,
      source ? ['Extracted by', element('code', {}, source.extract_command)] : null,
      source ? ['Reproducible in CI', source.reproducible_in_ci ? 'yes' : 'no'] : null,
    ]),
    source ? element('p', { class: 'note' }, source.note) : null,
  )
}

export function replayPanel(
  replay: ReplayArtifact,
  depth: DepthArtifact,
  source: SourceArtifact | null,
): HTMLElement {
  const spread =
    depth.bid_price !== null && depth.ask_price !== null
      ? `${price(depth.ask_price - depth.bid_price, depth.price_scale)} wide`
      : 'one side was empty'

  return element(
    'div',
    { class: 'panel-body' },
    element(
      'div',
      { class: 'columns' },
      element(
        'div',
        { class: 'column' },
        element('h3', {}, 'What was in the tape'),
        scrollBox(messageTable(replay)),
        element(
          'p',
          { class: 'note' },
          `${integer(replay.messages.applied)} messages applied out of ${integer(
            replay.parse.messages,
          )} parsed, ${integer(replay.parse.unknown_types)} of an unrecognised type. ` +
            `Parse status ${replay.parse.status}, ${bytes(replay.parse.bytes_consumed)} consumed of ${bytes(
              replay.provenance.source_bytes,
            )}.`,
        ),
      ),
      element(
        'div',
        { class: 'column' },
        element('h3', {}, 'The book at its deepest'),
        element(
          'p',
          { class: 'note' },
          `Snapshot taken after message ${integer(depth.captured_at_message)}, ` +
            `${depth.captured_at_peak_depth ? 'the moment the live order count peaked' : 'the end of the replay'}, ` +
            `holding ${integer(depth.live_orders_at_capture)} live orders across ` +
            `${integer(depth.occupied_bid_levels)} bid and ${integer(depth.occupied_ask_levels)} ask levels. ` +
            `Touch ${price(depth.bid_price, depth.price_scale)} by ${price(
              depth.ask_price,
              depth.price_scale,
            )}, ${spread}. ` +
            `A full trading day ends with the book emptied by the session close, so the ladder is anchored here rather than at the end.`,
        ),
        scrollBox(ladder(depth)),
      ),
    ),

    element('h3', {}, 'Would the shipped defaults have held'),
    sizingBlock(replay),
    provenanceBlock(replay, source),
  )
}
