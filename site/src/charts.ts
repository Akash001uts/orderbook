import uPlot from 'uplot'
import type { Options, AlignedData } from 'uplot'

import { element } from './format'

// uPlot wrappers.
//
// uPlot rather than a charting framework because this page draws four line charts
// and one of them has seven points. Forty kilobytes that does exactly this is a
// better trade than a hundred and fifty that does everything.
//
// Every chart here resizes with its container. Most people open a shared link on
// a phone, so a fixed pixel width would be the single most visible defect on the
// page.

const AXIS_COLOUR = '#5c6470'
const GRID_COLOUR = 'rgba(120, 132, 148, 0.18)'
const LABEL_COLOUR = '#9aa4b2'

export const SERIES_COLOURS = ['#7dd3fc', '#f0abfc', '#fcd34d', '#86efac', '#fca5a5']

interface ChartSpec {
  title?: string
  data: AlignedData
  series: Array<{ label: string; colour: string; dash?: number[] }>
  xLabel?: string
  yLabel?: string
  logX?: boolean
  height?: number
  xValues?: (self: uPlot, ticks: number[]) => string[]
  yValues?: (self: uPlot, ticks: number[]) => string[]
  /** Drawn as a horizontal rule, which is how the markout sign flip is marked. */
  zeroLine?: boolean
}

function baseOptions(spec: ChartSpec, width: number): Options {
  const height = spec.height ?? 260

  const options: Options = {
    width,
    height,
    padding: [12, 16, 4, 4],
    legend: { show: true, live: true },
    cursor: { drag: { x: false, y: false } },
    scales: {
      x: spec.logX ? { distr: 3 } : { time: false },
    },
    axes: [
      {
        stroke: AXIS_COLOUR,
        grid: { stroke: GRID_COLOUR, width: 1 },
        ticks: { stroke: GRID_COLOUR, width: 1 },
        font: '11px ui-monospace, monospace',
        labelFont: '11px ui-monospace, monospace',
        ...(spec.xLabel ? { label: spec.xLabel } : {}),
        ...(spec.xValues ? { values: spec.xValues } : {}),
      },
      {
        stroke: AXIS_COLOUR,
        grid: { stroke: GRID_COLOUR, width: 1 },
        ticks: { stroke: GRID_COLOUR, width: 1 },
        font: '11px ui-monospace, monospace',
        labelFont: '11px ui-monospace, monospace',
        ...(spec.yLabel ? { label: spec.yLabel } : {}),
        ...(spec.yValues ? { values: spec.yValues } : {}),
      },
    ],
    series: [
      { label: spec.xLabel ?? 'x' },
      ...spec.series.map((entry) => ({
        label: entry.label,
        stroke: entry.colour,
        width: 2,
        points: { show: true, size: 5, stroke: entry.colour, fill: entry.colour },
        ...(entry.dash ? { dash: entry.dash } : {}),
      })),
    ],
  }

  if (spec.zeroLine) {
    options.hooks = {
      draw: [
        (self) => {
          const y = self.valToPos(0, 'y', true)
          const context = self.ctx
          context.save()
          context.strokeStyle = LABEL_COLOUR
          context.setLineDash([4, 4])
          context.lineWidth = 1
          context.beginPath()
          context.moveTo(self.bbox.left, y)
          context.lineTo(self.bbox.left + self.bbox.width, y)
          context.stroke()
          context.restore()
        },
      ],
    }
  }

  return options
}

/** Renders into a container that owns its own width, and redraws on resize. */
export function chart(spec: ChartSpec): HTMLElement {
  const container = element('div', { class: 'chart' })
  if (spec.title) {
    container.append(element('h4', { class: 'chart-title' }, spec.title))
  }
  const host = element('div', { class: 'chart-host' })
  container.append(host)

  let plot: uPlot | null = null

  // A chart that fails should say so rather than leave a blank rectangle. A blank
  // rectangle on a page whose whole argument is "the numbers are checked" is the
  // worst possible failure mode.
  const render = () => {
    const width = Math.max(280, host.clientWidth || container.clientWidth || 640)
    try {
      if (plot) {
        plot.setSize({ width, height: spec.height ?? 260 })
        return
      }
      plot = new uPlot(baseOptions(spec, width), spec.data, host)
    } catch (error) {
      host.replaceChildren(
        element('p', { class: 'note' }, `chart failed to render: ${String(error)}`),
      )
      console.error('chart failed', spec.title, error)
    }
  }

  // A microtask rather than an animation frame, and this is not a style choice.
  // The host has no width until it is in the document, so the first render has to
  // wait for its caller to attach it. requestAnimationFrame and ResizeObserver
  // both deliver as part of the browser's rendering steps, and those steps do not
  // run at all in a tab that is not being composited: a background tab, or a
  // window the reader switched away from while the page loaded. Either would leave
  // every chart permanently blank. A microtask runs at the end of the current
  // task, by which point the caller's replaceChildren has attached the host, and
  // it runs whether or not anything is on screen.
  queueMicrotask(render)

  const observer = new ResizeObserver(() => {
    if (host.clientWidth > 0) render()
  })
  observer.observe(host)

  return container
}

/** A labelled horizontal bar, used for the P&L attribution and the depth ladder.
 *  A bar chart library for something this simple would be an odd trade. */
export function bar(
  label: string,
  value: string,
  fraction: number,
  className: string,
): HTMLElement {
  const width = `${Math.max(0, Math.min(1, fraction)) * 100}%`
  return element(
    'div',
    { class: `bar-row ${className}` },
    element('span', { class: 'bar-label' }, label),
    element(
      'span',
      { class: 'bar-track' },
      element('span', { class: 'bar-fill', style: `width: ${width}` }),
    ),
    element('span', { class: 'bar-value' }, value),
  )
}
