// Formatting, and the small DOM helpers the panels build themselves out of.
//
// Numbers use a thin space between groups rather than a comma, matching the
// documents this site is a front end for. A comma between digits in a table of
// prices and quantities reads as a decimal point to half the world.

const THIN_SPACE = ' '

export function integer(value: number): string {
  return Math.round(value)
    .toString()
    .replace(/\B(?=(\d{3})+(?!\d))/g, THIN_SPACE)
}

export function signedInteger(value: number): string {
  const text = integer(Math.abs(value))
  if (value > 0) return `+${text}`
  if (value < 0) return `-${text}`
  return text
}

export function decimal(value: number, places: number): string {
  const fixed = value.toFixed(places)
  const [whole = '0', fraction] = fixed.split('.')
  const grouped = whole.replace('-', '').replace(/\B(?=(\d{3})+(?!\d))/g, THIN_SPACE)
  const sign = whole.startsWith('-') ? '-' : ''
  return fraction === undefined ? `${sign}${grouped}` : `${sign}${grouped}.${fraction}`
}

export function signedDecimal(value: number, places: number): string {
  const text = decimal(Math.abs(value), places)
  if (value > 0) return `+${text}`
  if (value < 0) return `-${text}`
  return text
}

/** Scaled ITCH price to dollars. price_scale is carried in every artifact so this
 *  never has to assume 10000. */
export function price(scaled: number | null, scale: number): string {
  if (scaled === null) return 'none'
  return `$${decimal(scaled / scale, 2)}`
}

export function percent(value: number, places = 2): string {
  return `${decimal(value, places)}%`
}

export function ratio(value: number, places = 2): string {
  return `${decimal(value, places)}x`
}

export function nanoseconds(value: number): string {
  if (value >= 1_000_000) return `${decimal(value / 1_000_000, 1)} ms`
  if (value >= 1_000) return `${decimal(value / 1_000, 1)} us`
  return `${integer(value)} ns`
}

export function duration(ns: number): string {
  const seconds = ns / 1e9
  if (seconds >= 3600) return `${decimal(seconds / 3600, 1)} h`
  if (seconds >= 60) return `${decimal(seconds / 60, 1)} min`
  if (seconds >= 1) return `${decimal(seconds, 1)} s`
  return `${decimal(seconds * 1000, 1)} ms`
}

export function bytes(value: number): string {
  const units = ['B', 'KiB', 'MiB', 'GiB']
  let scaled = value
  let unit = 0
  while (scaled >= 1024 && unit < units.length - 1) {
    scaled /= 1024
    unit += 1
  }
  return `${decimal(scaled, unit === 0 ? 0 : 2)} ${units[unit]}`
}

export function shortHash(hash: string | null): string {
  return hash === null ? 'unhashed' : `${hash.slice(0, 12)}...`
}

// ---------------------------------------------------------------------------
// DOM helpers. Deliberately tiny: this page has no state to reconcile, it
// rebuilds a panel outright when the symbol changes, so a framework would be
// carrying a diffing engine to redraw eight tables.
// ---------------------------------------------------------------------------

type Child = Node | string | null | undefined | false

export function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attributes: Record<string, string> = {},
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag)
  for (const [name, value] of Object.entries(attributes)) {
    if (name === 'class') {
      node.className = value
    } else {
      node.setAttribute(name, value)
    }
  }
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue
    node.append(typeof child === 'string' ? document.createTextNode(child) : child)
  }
  return node
}

export function text(tag: keyof HTMLElementTagNameMap, className: string, content: string): HTMLElement {
  return element(tag, { class: className }, content)
}

/** A two column definition table, which is most of this page. */
export function statTable(rows: Array<[string, string | Node] | null>): HTMLElement {
  const body = element('tbody')
  for (const row of rows) {
    if (row === null) continue
    const [label, value] = row
    body.append(
      element(
        'tr',
        {},
        element('th', { scope: 'row' }, label),
        element('td', {}, typeof value === 'string' ? document.createTextNode(value) : value),
      ),
    )
  }
  return element('table', { class: 'stats' }, body)
}

/** Wraps wide content so it scrolls inside its own box. The page body must never
 *  scroll sideways: most people open a shared link on a phone, and a table that
 *  drags the whole document wide is the most visible defect a page can have. */
export function scrollBox(child: Node): HTMLElement {
  return element('div', { class: 'scroll-x' }, child)
}

export function section(id: string, title: string, subtitle?: string): HTMLElement {
  return element(
    'section',
    { id, class: 'panel' },
    element('h2', {}, title),
    subtitle ? element('p', { class: 'subtitle' }, subtitle) : null,
  )
}

export function note(content: string): HTMLElement {
  return element('p', { class: 'note' }, content)
}
