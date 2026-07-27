# Strategy results

Phase 5. A two sided market maker quoting into a NASDAQ TotalView-ITCH capture
replayed order by order, with its own quotes resting in the same reconstructed
book and contending for real queue position.

**Read the fill model before the P&L.** Every number here is downstream of it, and
an over-optimistic fill model would make all of them meaningless. The model is
stated in full below and implemented in `strategy/include/strategy/queue_position.hpp`.

## What fills a strategy order, and what deliberately does not

Three rules, and the third is the one that matters most.

1. **A trade at our price.** Shares consume the level from the front. The quantity
   estimated ahead of us absorbs first and only the excess reaches us.
2. **A trade through our price.** A resting order on our side at a price strictly
   worse than ours was executed. With our order genuinely present the aggressor
   would have taken us first, so we fill for the traded quantity, capped at our
   size. This is the more inferential of the two paths, so the report counts these
   fills separately: **8 of 38 in the baseline run.**
3. **Nothing else.** In particular, a passive venue add that crosses our resting
   quote does **not** fill us.

Rule 3 is the single most consequential decision in this phase and it is argued in
full in DESIGN.md, "The crossed book, and why the strategy tolerates one". The
short version: an ITCH `A` message is a passive post, not an aggressive order.
Orders that actually took liquidity appear as `E` or `C` executions. Filling on a
passive post would invent liquidity taking that the tape says did not happen, at
our price, in our favour, every time it occurred. It is the most flattering
assumption available anywhere in this project, which is exactly why it is refused.

The cost of refusing it is measured rather than assumed. See "The crossed book"
below.

## Queue position is an estimate, and here is precisely why

Initialised to the level's resting quantity at the moment of insertion, which is
exactly the quantity holding time priority over us. Then decremented by observed
executions, and adjusted for observed cancels under the **uniform cancel
assumption**: when C shares are cancelled at our price with A estimated ahead and B
behind, A is reduced by `C * A / (A + B)`.

The part that is genuinely unknowable is narrower than it is usually stated, and
worth being precise about. Order-by-order data does tell us where every real order
sits, and this project reconstructs exactly that. What it cannot tell us is where
*our* order would have sat, because our order is hypothetical: it never existed, so
no venue ever gave it a place, and every order that arrived after our notional
insertion queued behind a book that did not contain us. The counterfactual queue is
not observable at any level of feed detail.

The two alternatives to the uniform assumption are both worse, and naming them is
the point:

| Assumption | Effect |
| --- | --- |
| All cancels land behind us | Maximally pessimistic, never improves our position, always understates fills |
| All cancels land ahead of us | Maximally optimistic, manufactures P&L |
| Uniform | Mildly optimistic, because real cancels skew toward recently placed orders and therefore toward the back |

Uniform is wrong in a knowable direction. It is not corrected, because correcting
it would require a parameter fitted to the data being tested, which is a worse sin
than the bias it removes.

## Baseline configuration

| | |
| --- | --- |
| Data | `data/qqq_slice.itch`, QQQ, 2019-12-30, 183 950 messages applied |
| Quote offset | 1 tick from mid |
| Quote size | 100 shares per side |
| Position limit | 1 000 shares |
| Inventory skew | 0, so the baseline has no inventory control at all |
| Fees | maker rebate 0.002 ticks per share, taker fee 0.003 |

P&L is in ticks times shares. One tick is one cent here, so divide by 100 for
dollars.

### P&L attribution

| Component | Ticks x shares | Dollars |
| --- | --- | --- |
| Spread capture | +2 755.00 | +27.55 |
| Inventory | **-51 096.00** | -510.96 |
| Fees and rebates | +5.90 | +0.06 |
| **Total** | **-48 335.10** | **-483.35** |

The decomposition is exact rather than approximate. Total mark to market P&L is
`cash + position * mid`, and only two things move it: at a fill with the mid held
still the change is `signed_quantity * (mid - fill_price)`, which is spread
capture, and between fills with the position held still the change is
`position * delta_mid`, which is inventory. Fees are booked as they occur. So
`total == spread + inventory + fees` holds to floating point exactly, and
`StrategyPnl.AttributionIsExact` asserts it rather than trusting it.

### Risk and activity

| | |
| --- | --- |
| Final position | 940 shares long |
| Max long / short | 998 / 0 |
| Max drawdown | 49 875.70 |
| Fills | 38, for 2 950 shares, of which 8 came from trade-through |
| Quotes placed / cancelled / post-only rejected | 4 808 / 4 784 / 19 |
| Fill ratio | 0.0061 of quoted shares |
| Sharpe | -0.008 on 20 046 samples at a 1 s interval, not annualised |

Sharpe is deliberately not annualised. Annualising a backtest that covers part of
one trading day would be arithmetic dressed up as a result.

### Markouts

Signed P&L per share against the mid at each horizon. Positive means the market
moved in our favour after the fill.

| Horizon | Buy | Sell | All | Fills |
| --- | --- | --- | --- | --- |
| 100 ms | +0.9458 | +0.5109 | +0.7976 | 38 |
| 1 s | +0.9874 | +0.5806 | +0.8488 | 38 |
| 10 s | +0.7560 | +0.4756 | +0.6605 | 38 |

Zero markouts were left unresolved at the end of the replay, so all three horizons
are fully measured rather than partially.

**These are positive, and that is the interesting result.** Persistently negative
markouts would mean the strategy is being adversely selected, picked off by
informed flow. That is not what is happening here. At every horizon out to ten
seconds the fills are good ones. The strategy still loses badly, and the reason is
entirely separate.

## What actually went wrong, and what it demonstrates

The baseline loses 48 335 while capturing 2 755 of spread. **Inventory is the whole
story**, and it is not a fill model artifact.

With skew set to zero there is no mechanism to reduce inventory. The strategy
accumulated a long position, reached 998 shares against its 1 000 limit, and held
it for essentially the entire session: 19 996 seconds of holding time. QQQ fell
roughly a dollar across that window. A ~1 000 share long through a ~100 tick
decline is a ~50 000 tick-share loss, which is what the inventory line says.

That the loss reconciles to the symbol's actual price move is a useful check that
the accounting is measuring something real rather than an artifact of the harness.

The markouts and the inventory line together say something a single P&L number
would hide: **the fills were fine and the position management was not.** Every
individual trade looked good ten seconds later. The strategy simply had no way to
get flat.

## Sensitivity analysis

**This is sensitivity analysis, not a search for an edge.** The grid covers one
symbol on one day. The best cell is a property of that day and nothing more, and
presenting it as a discovered edge would be worse than omitting the sweep
entirely. What is worth reading is the shape.

Run with `ob_strategy_backtest --sweep`.

| Offset | Skew | Size | Total | Spread | Inventory | Fills | 1 s markout | Crossed % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 0 | 100 | -48 335 | 2 755 | -51 096 | 38 | +0.85 | 0.014 |
| 1 | 0 | 300 | -54 619 | 4 295 | -58 924 | 42 | +0.72 | 0.016 |
| 1 | 1 | 100 | -20 021 | 2 757 | -22 784 | 40 | +0.83 | 0.014 |
| 1 | 1 | 300 | -22 269 | 5 573 | -27 854 | 47 | +0.78 | 0.021 |
| 1 | 2 | 100 | **-12 021** | 2 707 | -14 734 | 40 | +0.82 | 0.015 |
| 1 | 2 | 300 | -17 445 | 5 440 | -22 897 | 44 | +0.81 | 0.025 |
| 2 | 0 | 100 | -4 300 | 150 | -4 450 | 1 | +0.50 | 0.011 |
| 2 | 2 | 300 | -299 | 900 | -1 200 | 5 | +1.00 | 0.010 |
| 4 | any | any | 0 | 0 | 0 | 0 | n/a | 0.010 |

Three things in the shape, none of which is a trading recommendation:

**Inventory skew does what it is supposed to.** At offset 1 and size 100, going
from skew 0 to 2 leaves spread capture essentially unchanged, 2 755 against 2 707,
while inventory improves from -51 096 to -14 734. The mechanism is visible directly
in the position: max long falls from 998 to 304, and the strategy starts taking
short positions at all, reaching -92. It earns the same edge per fill and stops
carrying the risk.

**Quoting away from the touch stops the strategy trading entirely.** At offset 2
there is 1 fill in the whole session; at offset 4 there are none. QQQ trades at a
one cent spread almost always, so a quote two ticks off mid is never at the touch
and never reaches the front of any queue. The apparent improvement in total P&L as
the offset widens is not an improvement, it is the strategy switching itself off. A
sweep read without the fill count would get this exactly backwards.

**Bigger quotes did not help.** Size 300 loses more than size 100 at every offset
and skew, because the extra size increases inventory faster than it increases
spread capture.

## The crossed book

The consequence of refusing to fill on a crossing passive add is that the
reconstructed book is sometimes crossed. That is measured, not tolerated silently.

| | Baseline |
| --- | --- |
| Crossed intervals | 26 |
| Total crossed time | 879 ms |
| Fraction of book updates crossed | 0.0141 % |
| Worst crossing depth | 26 ticks |

A crossed interval is time the strategy's quote sat inside the real spread with
nobody trading against it. It is exactly where an optimistic backtest would hide
its optimism, so counting it converts an unfalsifiable assumption into a number a
reader can check. At 0.014 percent of updates and under a second in total, this
result does not depend materially on the assumption. **A configuration that spent a
large fraction of its time crossed would be one whose P&L should not be quoted**,
and the sweep reports the figure for every cell so that can be checked rather than
assumed.

## Which results depend on the fill model

Worth separating carefully, because not every result leans on the same assumption.

**Depends heavily on the fill model:** the fill count, the fill ratio, the spread
capture line, and every markout. All of these are computed only from fills, and
fills are what the model produces. The 8 trade-through fills of 38 are the most
model-dependent subset, because those come from an inference about what would have
happened rather than from a trade at our price.

**Depends weakly:** inventory P&L. It is dominated by the size of the position and
the size of the market's move, and while the model determines how the position was
acquired, an inventory loss of this magnitude follows from holding roughly a
thousand shares through a dollar decline under any plausible fill model.

**Does not depend on it at all:** the crossed book statistics, the quote and cancel
counts, and the message counts. Those describe what the strategy did and what the
venue did, not what it was assumed to have been filled on.

**The direction of the remaining bias is stated rather than hidden.** The uniform
cancel assumption is mildly optimistic, so the true fill count is probably slightly
lower than reported. Since the strategy loses money on inventory rather than on
fills, a lower fill count would improve the result, not worsen it. The bias
therefore works against the conclusion drawn here, which is the safe direction for
it to run.

## Reproduction

```bash
cmake --preset release
cmake --build --preset release
./out/build/release/ob_strategy_backtest --file data/qqq_slice.itch --symbol QQQ --offset 1 --size 100 --limit 1000 --skew 0
./out/build/release/ob_strategy_backtest --file data/qqq_slice.itch --sweep
```
