// The shape of the committed artifacts, and the loader that fetches them.
//
// These types are hand written against site/data/SCHEMA.md rather than generated,
// because the schema is the contract and a generator would make the contract
// invisible. If a field here disagrees with that document, that document wins.
//
// Everything is fetched on demand and cached by URL. A visitor who never opens a
// symbol never downloads it, and clicking back to a symbol already seen costs
// nothing.

export interface Provenance {
  source: string
  source_bytes: number
  source_sha256: string | null
  command: string
}

export interface ArtifactHeader {
  schema: number
  tool: string
  version: string
  provenance: Provenance
}

export interface ReplayArtifact extends ArtifactHeader {
  symbol: string
  config: {
    band_levels: number
    arena_capacity: number
    max_cold_levels_per_side: number
    tick_size: number
    price_scale: number
    message_limit: number
  }
  parse: {
    status: number
    messages: number
    bytes_consumed: number
    unknown_types: number
  }
  locate: { resolved: boolean; code: number }
  messages: {
    applied: number
    other_symbol: number
    adds: number
    executions: number
    cancels: number
    deletes: number
    replaces: number
    trades: number
    cross_trades: number
    broken_trades: number
    system_events: number
  }
  rejections: {
    off_tick_prices: number
    rejected_adds: number
    unknown_order_references: number
  }
  book: {
    final_resting_orders: number
    occupied_bid_levels: number
    occupied_ask_levels: number
    rebases: number
    rebases_abandoned: number
    cold_levels: number
    cold_operations: number
  }
  sizing: {
    peak_live_orders: number
    peak_live_orders_at_message: number
    arena_capacity: number
    library_arena_default: number
    peak_occupied_levels: number
    band_levels: number
    touch_low_tick: number | null
    touch_high_tick: number | null
    touch_span_ticks: number | null
    touch_span_band_percent: number | null
  }
  final_touch: {
    bid_tick: number | null
    bid_price: number | null
    ask_tick: number | null
    ask_price: number | null
  }
}

export interface DepthLevel {
  tick: number
  price: number
  quantity: number
  orders: number
}

export interface DepthArtifact extends ArtifactHeader {
  symbol: string
  levels_per_side: number
  price_scale: number
  tick_size: number
  captured_at_message: number
  captured_at_peak_depth: boolean
  live_orders_at_capture: number
  occupied_bid_levels: number
  occupied_ask_levels: number
  bid_tick: number | null
  bid_price: number | null
  ask_tick: number | null
  ask_price: number | null
  bids: DepthLevel[]
  asks: DepthLevel[]
}

export interface Markout {
  horizon: string
  horizon_ns: number
  buy_per_share: number | null
  sell_per_share: number | null
  all_per_share: number | null
  fills: number
}

export interface BacktestArtifact extends ArtifactHeader {
  config: {
    symbol: string
    quote_offset_ticks: number
    quote_size: number
    position_limit: number
    inventory_skew_ticks: number
    latency_us: number
  }
  replay: { messages_seen: number; messages_applied: number }
  pnl: { spread: number; inventory: number; fees: number; total: number; unit: string }
  risk: {
    final_position: number
    max_long: number
    max_short: number
    time_holding_ns: number
    max_drawdown: number
  }
  activity: {
    fills: number
    filled_shares: number
    quoted_shares: number
    trade_through_fills: number
    quotes_placed: number
    quotes_cancelled: number
    quotes_rejected: number
    cancels_too_late: number
    limit_overshoot_shares: number
    fill_ratio: number
  }
  sharpe: {
    value: number | null
    samples: number
    informative_samples: number
    clears_minimum_events: boolean
    annualised: boolean
  }
  markouts: Markout[]
  unresolved_markouts: number
  crossed_book: {
    intervals: number
    total_ns: number
    observations: number
    crossed_observations: number
    locked_observations: number
    worst_depth_ticks: number
    crossed_percent: number
  }
}

export interface LatencySweepRow {
  latency_us: number
  total_pnl: number
  spread: number
  inventory: number
  fills: number
  filled_shares: number
  fill_ratio: number
  quotes_placed: number
  markout_1s_per_share: number | null
  crossed_percent: number
}

export interface ParameterSweepRow {
  offset_ticks: number
  skew_ticks: number
  size: number
  total_pnl: number
  spread: number
  inventory: number
  fees: number
  fills: number
  fill_ratio: number
  max_drawdown: number
  markout_1s_per_share: number | null
  crossed_percent: number
}

export interface SweepsArtifact extends ArtifactHeader {
  config: BacktestArtifact['config']
  parameter_sweep?: ParameterSweepRow[]
  latency_sweep?: LatencySweepRow[]
}

export interface SourceArtifact {
  schema: number
  symbol: string
  capture: string
  capture_bytes: number
  capture_sha256: string
  extract_command: string
  slice: string
  slice_bytes: number
  slice_sha256: string
  reproducible_in_ci: boolean
  note: string
}

export interface IndexEntry {
  symbol: string
  messages_applied: number
  peak_live_orders: number
  library_arena_default: number
  bid_price: number | null
  ask_price: number | null
  price_scale: number
  touch_span_ticks: number | null
  baseline_total_pnl: number
  fills: number
  off_tick_prices: number
  rejected_adds: number
  rebases: number
  cold_levels: number
  reproducible_in_ci: boolean
}

export interface IndexArtifact {
  schema: number
  generated_by: string
  default_symbol: string
  symbols: IndexEntry[]
}

export interface LatencyRow {
  bucket: string
  count: number
  p50: number
  p90: number
  p99: number
  p99_9: number
  p99_99: number
  max: number
  mean: number
}

export interface LatencyArtifact extends ArtifactHeader {
  environment: {
    cpu_brand: string
    hardware_threads: number
    os: string
    compiler: string
    build_type: string
    build_flags: string
  }
  clock: {
    source: string
    ticks_per_ns: number
    cpuid_available: boolean
    tsc_invariant: boolean
    fallback_reason: string
    overhead_ns: number
    overhead_subtracted: boolean
  }
  pinning: { status: string; cpu: number; detail: string }
  frequency_scaling: { status: string; detail: string }
  degraded_conditions: number
  input: {
    symbol: string
    arena_capacity: number
    warmup_runs: number
    measured_runs: number
    messages_per_run: number
  }
  latency_ns: LatencyRow[]
  max_is_a_scheduling_event: boolean
  throughput: {
    mean_messages_per_second: number
    run_to_run_cv_percent: number
    measured_uninstrumented: boolean
    runs: number[]
  }
}

/** Everything one symbol's panels need. */
export interface SymbolBundle {
  replay: ReplayArtifact
  depth: DepthArtifact
  backtest: BacktestArtifact
  sweeps: SweepsArtifact
  source: SourceArtifact | null
}

// The base is relative so the site works from a subdirectory as happily as from a
// domain root, which matters because a portfolio may host it either way.
const BASE = new URL('.', document.baseURI).href

const cache = new Map<string, Promise<unknown>>()

function fetchJson<T>(path: string): Promise<T> {
  const existing = cache.get(path)
  if (existing) {
    return existing as Promise<T>
  }
  const pending = fetch(new URL(path, BASE).href).then((response) => {
    if (!response.ok) {
      throw new Error(`${path} returned ${response.status}`)
    }
    return response.json() as Promise<T>
  })
  cache.set(path, pending)
  return pending
}

export function loadIndex(): Promise<IndexArtifact> {
  return fetchJson<IndexArtifact>('index.json')
}

export function loadLatency(): Promise<LatencyArtifact> {
  return fetchJson<LatencyArtifact>('bench/latency.json')
}

export async function loadSymbol(symbol: string): Promise<SymbolBundle> {
  const directory = `symbols/${symbol}`
  const [replay, depth, backtest, sweeps, source] = await Promise.all([
    fetchJson<ReplayArtifact>(`${directory}/replay.json`),
    fetchJson<DepthArtifact>(`${directory}/depth.json`),
    fetchJson<BacktestArtifact>(`${directory}/backtest.json`),
    fetchJson<SweepsArtifact>(`${directory}/sweeps.json`),
    // A symbol generated straight from a committed slice has no extraction step
    // to record, so a missing source.json is a legal state rather than an error.
    fetchJson<SourceArtifact>(`${directory}/source.json`).catch(() => null),
  ])
  return { replay, depth, backtest, sweeps, source }
}
