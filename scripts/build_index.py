#!/usr/bin/env python3
"""Builds site/data/index.json from the committed per symbol artifacts.

The symbol selector needs a button and a one line summary per symbol before any
detail file loads, and that summary has to come from somewhere. Reading it out of
the artifacts rather than typing it into markup means the site cannot disagree with
its own data.

This is a pure function of files already in the repository, so CI can rerun it and
diff the result even though it cannot regenerate the artifacts themselves. That
closes the one gap the provenance rule leaves: the artifacts for most symbols
cannot be reproduced by a runner, but the index derived from them can.

Usage:
  python scripts/build_index.py [--root site/data] [--check]

--check rebuilds into memory and compares against the committed file rather than
writing, which is what the CI job runs.
"""

import argparse
import json
import sys
from pathlib import Path

SCHEMA_VERSION = 1

# The curated set, in the order the selector shows them. Chosen for variety rather
# than for rank: three index ETFs that were already measured for the arena
# derivation, three mega caps spanning a factor of three hundred in share price, two
# mid priced high message rate semiconductors, and one low priced name. See
# Frontend plan, phase F2. More symbols would be more scrolling, not more signal.
DEFAULT_ORDER = ["QQQ", "SPY", "IWM", "AAPL", "MSFT", "GOOGL", "AMD", "INTC", "NIO"]

DEFAULT_SYMBOL = "QQQ"


def load(path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def entry_for(directory):
    """One index row, read out of that symbol's own artifacts."""
    replay = load(directory / "replay.json")
    backtest = load(directory / "backtest.json")
    depth = load(directory / "depth.json")

    source_path = directory / "source.json"
    source = load(source_path) if source_path.exists() else {}

    sizing = replay["sizing"]

    # The touch comes from the depth snapshot, not from the end of the replay. A
    # full trading day closes with the book emptied, so the final touch of a whole
    # day is null for every symbol, which is correct and useless in a summary line.
    # The snapshot is taken where the book was deepest, so its touch is a price the
    # symbol actually traded around.
    return {
        "symbol": replay["symbol"],
        "messages_applied": replay["messages"]["applied"],
        "peak_live_orders": sizing["peak_live_orders"],
        "library_arena_default": sizing["library_arena_default"],
        "bid_price": depth["bid_price"],
        "ask_price": depth["ask_price"],
        "price_scale": replay["config"]["price_scale"],
        "touch_span_ticks": sizing["touch_span_ticks"],
        "baseline_total_pnl": backtest["pnl"]["total"],
        "fills": backtest["activity"]["fills"],
        "off_tick_prices": replay["rejections"]["off_tick_prices"],
        "rejected_adds": replay["rejections"]["rejected_adds"],
        "rebases": replay["book"]["rebases"],
        "cold_levels": replay["book"]["cold_levels"],
        "reproducible_in_ci": bool(source.get("reproducible_in_ci", False)),
    }


def build(root):
    symbols_root = root / "symbols"
    if not symbols_root.is_dir():
        raise SystemExit(f"no symbol directories under {symbols_root}")

    present = {path.name for path in symbols_root.iterdir() if path.is_dir()}

    # Ordered set first, then anything else alphabetically, so adding a symbol
    # without touching this file still produces a valid index.
    ordered = [name for name in DEFAULT_ORDER if name in present]
    ordered += sorted(present - set(ordered))

    entries = []
    for name in ordered:
        directory = symbols_root / name
        if not (directory / "replay.json").exists():
            print(f"skipping {name}, no replay.json", file=sys.stderr)
            continue
        entries.append(entry_for(directory))

    if not entries:
        raise SystemExit("no complete symbol directories found")

    default = DEFAULT_SYMBOL
    if all(item["symbol"] != default for item in entries):
        default = entries[0]["symbol"]

    return {
        "schema": SCHEMA_VERSION,
        "generated_by": "scripts/build_index.py",
        "default_symbol": default,
        "symbols": entries,
    }


def render(index):
    """Two space indentation and a trailing newline, matching the C++ writer so a
    diff of one file reads the same as a diff of the other."""
    return json.dumps(index, indent=2) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="site/data")
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    root = Path(arguments.root)
    text = render(build(root))
    target = root / "index.json"

    if not arguments.check:
        target.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {target}")
        return 0

    if not target.exists():
        print(f"{target} is missing", file=sys.stderr)
        return 1

    committed = target.read_text(encoding="utf-8")
    if committed == text:
        print(f"{target} matches the artifacts it is derived from")
        return 0

    print(f"{target} disagrees with the per symbol artifacts", file=sys.stderr)
    print("rerun scripts/build_index.py and commit the result", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
