import 'uplot/dist/uPlot.min.css'
import './styles.css'

import { loadBaseline, loadIndex, loadLatency, loadSymbol } from './data'
import type { BaselineArtifact, IndexArtifact, IndexEntry, SymbolBundle } from './data'
import { LINKS, PROJECT, REPO_URL } from './content'
import { decimal, element, integer, price, signedDecimal } from './format'
import { heroPanel } from './panels/hero'
import { replayPanel } from './panels/replay'
import { backtestPanel } from './panels/backtest'
import { correctnessPanel } from './panels/correctness'
import { measurementPanel } from './panels/measurement'

// One long scrolling page. The only interactivity is the symbol selector, which
// swaps the data behind two panels without a reload and keeps the chosen symbol in
// the URL hash so a link can point at a specific stock and survive a hard refresh.

const app = document.querySelector<HTMLElement>('#app')!

function symbolFromHash(index: IndexArtifact): string {
  const requested = decodeURIComponent(window.location.hash.replace(/^#/, '')).toUpperCase()
  const known = index.symbols.some((entry) => entry.symbol === requested)
  return known ? requested : index.default_symbol
}

function selectorButton(entry: IndexEntry, active: boolean): HTMLElement {
  const touch =
    entry.bid_price === null
      ? 'no two sided touch'
      : price(entry.bid_price, entry.price_scale)

  return element(
    'button',
    {
      class: `symbol ${active ? 'active' : ''}`,
      type: 'button',
      'data-symbol': entry.symbol,
      'aria-pressed': active ? 'true' : 'false',
    },
    element('span', { class: 'symbol-ticker' }, entry.symbol),
    element(
      'span',
      { class: 'symbol-summary' },
      `${touch} / ${decimal(entry.messages_applied / 1e6, 2)}M msgs / peak ${integer(
        entry.peak_live_orders,
      )}`,
    ),
    element(
      'span',
      { class: `symbol-pnl ${entry.baseline_total_pnl >= 0 ? 'good' : 'bad'}` },
      `${signedDecimal(entry.baseline_total_pnl, 0)} over ${integer(entry.fills)} fills`,
    ),
  )
}

function linksPanel(): HTMLElement {
  return element(
    'section',
    { class: 'panel', id: 'links' },
    element('h2', {}, 'The documents behind this page'),
    element(
      'p',
      { class: 'lede' },
      'This site is a front end for four long documents. They carry the reasoning, the alternatives that were rejected, and the parts that did not work.',
    ),
    element(
      'ul',
      { class: 'links' },
      ...LINKS.map((link) =>
        element(
          'li',
          {},
          element(
            'a',
            {
              class: 'link-name',
              href: `${REPO_URL}/blob/main/${link.file}`,
              target: '_blank',
              rel: 'noopener',
            },
            link.file,
          ),
          element('span', { class: 'link-blurb' }, link.blurb),
        ),
      ),
    ),
    element(
      'p',
      { class: 'note' },
      element('span', {}, 'The repository is at '),
      element('a', { href: REPO_URL, target: '_blank', rel: 'noopener' }, REPO_URL),
      element(
        'span',
        {},
        '. It holds the C++ engine and the full artifact set this page reads, ' +
          'and every document above links straight into it.',
      ),
    ),
  )
}

function errorPanel(error: unknown): HTMLElement {
  return element(
    'section',
    { class: 'panel error' },
    element('h2', {}, 'Could not load the artifacts'),
    element('p', {}, String(error)),
    element(
      'p',
      { class: 'note' },
      'This page reads JSON from the same origin. If you are opening it from the filesystem rather than over http, the browser will block those requests.',
    ),
  )
}

async function main(): Promise<void> {
  let index: IndexArtifact
  let latency: Awaited<ReturnType<typeof loadLatency>>
  let baseline: BaselineArtifact

  try {
    ;[index, latency, baseline] = await Promise.all([loadIndex(), loadLatency(), loadBaseline()])
  } catch (error) {
    app.replaceChildren(errorPanel(error))
    return
  }

  let current = symbolFromHash(index)

  const buttons = element('div', { class: 'selector' })
  const replayHost = element('section', { class: 'panel', id: 'replay' })
  const backtestHost = element('section', { class: 'panel', id: 'backtest' })

  const symbolsSection = element(
    'section',
    { class: 'panel', id: 'symbols' },
    element('h2', {}, 'One symbol at a time'),
    element(
      'p',
      { class: 'lede' },
      `Nine symbols from a single ordinary session, ${PROJECT.captureDate}, each a complete trading day. ` +
        'Chosen for spread rather than for rank: share prices across a factor of three hundred, message counts across a factor of three, ' +
        'and peak depth across a factor of sixty. Where a symbol’s results are strange, this page says so rather than smoothing it.',
    ),
    buttons,
  )

  const renderSymbol = (bundle: SymbolBundle) => {
    replayHost.replaceChildren(
      element('h2', {}, `Replay: ${bundle.replay.symbol}`),
      replayPanel(bundle.replay, bundle.depth, bundle.source),
    )
    backtestHost.replaceChildren(
      element('h2', {}, `Market making: ${bundle.replay.symbol}`),
      backtestPanel(bundle.backtest, bundle.sweeps),
    )
  }

  const renderButtons = () => {
    buttons.replaceChildren(
      ...index.symbols.map((entry) => selectorButton(entry, entry.symbol === current)),
    )
  }

  const select = async (symbol: string, updateHash: boolean) => {
    current = symbol
    renderButtons()
    if (updateHash) {
      window.location.hash = symbol
    }
    replayHost.replaceChildren(element('h2', {}, `Replay: ${symbol}`), element('p', { class: 'loading' }, 'loading'))
    backtestHost.replaceChildren(element('h2', {}, `Market making: ${symbol}`), element('p', { class: 'loading' }, 'loading'))
    try {
      const bundle = await loadSymbol(symbol)
      // A visitor who clicks quickly can outrun a fetch. Only the latest
      // selection is allowed to paint.
      if (current !== symbol) return
      renderSymbol(bundle)
    } catch (error) {
      replayHost.replaceChildren(errorPanel(error))
      backtestHost.replaceChildren()
    }
  }

  buttons.addEventListener('click', (event) => {
    const target = (event.target as HTMLElement).closest('button[data-symbol]')
    if (!target) return
    const symbol = target.getAttribute('data-symbol')
    if (symbol && symbol !== current) void select(symbol, true)
  })

  window.addEventListener('hashchange', () => {
    const symbol = symbolFromHash(index)
    if (symbol !== current) void select(symbol, false)
  })

  app.replaceChildren(
    heroPanel(latency, baseline),
    symbolsSection,
    replayHost,
    backtestHost,
    correctnessPanel(),
    measurementPanel(index),
    linksPanel(),
    element(
      'footer',
      {},
      element(
        'p',
        {},
        `Built from artifact schema 1. Every per symbol figure is read from JSON at load time. ` +
          `The QQQ set generated from the committed slice is regenerated and diffed on every push; the rest come from a ${PROJECT.captureDate} capture that carries its hash in every file.`,
      ),
      element('p', {}, element('a', { href: '#top' }, 'Back to the top')),
    ),
  )

  await select(current, false)
}

void main()
