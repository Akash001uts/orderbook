#!/usr/bin/env python3
"""Render the baseline and allocation tables in BENCHMARKS.md from the canonical
JSON artifact.

site/data/bench/baseline.json is the single source for the std::map comparison. The
site reads it directly; this script keeps the prose document in step with it so the
two can never disagree. It edits only the regions bracketed by the GENERATED
markers and leaves every surrounding paragraph untouched.

Two modes:

    python3 scripts/sync_benchmarks.py            update the marked sections in place
    python3 scripts/sync_benchmarks.py --check    exit non-zero if they are stale

The --check mode is what CI runs. It also validates the artifact, so a malformed
baseline fails the build rather than silently producing an empty table.

Standard library only, so it runs on any runner without a dependency step.
"""

import argparse
import json
import math
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = REPO_ROOT / "site" / "data" / "bench" / "baseline.json"
DOCUMENT = REPO_ROOT / "BENCHMARKS.md"

REQUIRED_ROW_IDS = ("add", "cancel", "match", "best_bid")

BASELINE_BEGIN = "<!-- BEGIN GENERATED baseline (scripts/sync_benchmarks.py) -->"
BASELINE_END = "<!-- END GENERATED baseline -->"
ALLOC_BEGIN = "<!-- BEGIN GENERATED allocations (scripts/sync_benchmarks.py) -->"
ALLOC_END = "<!-- END GENERATED allocations -->"


class BaselineError(Exception):
    """Raised when the artifact does not satisfy the schema this script expects."""


def number(value):
    """Format a number the way the tables do: no decimal point on whole values,
    and no trailing zeros otherwise, so the rendered bytes are stable."""
    if value == int(value):
        return str(int(value))
    return "%g" % value


def load_artifact(path):
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as error:
        raise BaselineError("cannot read %s: %s" % (path, error))
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as error:
        raise BaselineError("%s is not valid JSON: %s" % (path, error))
    return validate(data)


def _require_string(record, field):
    value = record.get(field)
    if not isinstance(value, str) or not value:
        raise BaselineError("%s must be a non-empty string" % field)
    return value


def validate(data):
    """Enforce the baseline schema on a parsed object. Kept separate from file I/O
    so the regression tests can exercise it on hand built inputs. Treats the input
    as unknown: a corrupt or truncated file is rejected rather than trusted."""
    if not isinstance(data, dict):
        raise BaselineError("top level must be an object")

    if data.get("schema") != 1:
        raise BaselineError("expected schema 1, found %r" % data.get("schema"))

    _require_string(data, "tool")
    _require_string(data, "version")
    _require_string(data, "benchmark_command")
    _require_string(data, "statistic")
    repetitions = data.get("repetitions")
    if not isinstance(repetitions, int) or isinstance(repetitions, bool) or repetitions <= 0:
        raise BaselineError("repetitions must be a positive integer")

    rows = data.get("rows")
    if not isinstance(rows, list) or not rows:
        raise BaselineError("rows must be a non-empty list")

    seen = set()
    for row in rows:
        if not isinstance(row, dict):
            raise BaselineError("every row must be an object")
        row_id = _require_string(row, "id")
        if row_id in seen:
            raise BaselineError("duplicate row id %r" % row_id)
        seen.add(row_id)
        _require_string(row, "label")
        for field in ("flat_ns", "naive_ns"):
            value = row.get(field)
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                raise BaselineError("row %r field %s is not a number" % (row_id, field))
            if not math.isfinite(value) or value <= 0:
                raise BaselineError(
                    "row %r field %s must be finite and positive, found %r"
                    % (row_id, field, value)
                )
        interpretation = row.get("interpretation")
        if interpretation not in ("faster", "below_resolution"):
            raise BaselineError(
                "row %r has unknown interpretation %r" % (row_id, interpretation)
            )
        # A "faster" row renders a ratio, so the flat book must genuinely be faster.
        # Without this a row where flat >= naive would render a misleading "0.8x
        # faster" from an interpretation that no longer matches the measurements.
        if interpretation == "faster" and row["naive_ns"] <= row["flat_ns"]:
            raise BaselineError(
                "row %r is marked faster but flat %r is not below naive %r"
                % (row_id, row["flat_ns"], row["naive_ns"])
            )

    missing = [row_id for row_id in REQUIRED_ROW_IDS if row_id not in seen]
    if missing:
        raise BaselineError("missing required row ids: %s" % ", ".join(missing))

    floor = data.get("query_noise_floor_ns")
    if not isinstance(floor, (int, float)) or isinstance(floor, bool):
        raise BaselineError("query_noise_floor_ns is required and must be a number")
    if not math.isfinite(floor) or floor < 0:
        raise BaselineError(
            "query_noise_floor_ns must be finite and non-negative, found %r" % floor
        )

    alloc = data.get("allocations_per_add")
    if not isinstance(alloc, dict) or "flat" not in alloc or "naive" not in alloc:
        raise BaselineError("allocations_per_add must carry flat and naive")
    for arm in ("flat", "naive"):
        value = alloc[arm]
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise BaselineError("allocations_per_add.%s is not a number" % arm)
        if not math.isfinite(value) or value < 0:
            raise BaselineError(
                "allocations_per_add.%s must be finite and non-negative, found %r"
                % (arm, value)
            )

    return data


def baseline_table(data, newline):
    lines = [
        "| Operation | Flat book | `std::map` book | Ratio |",
        "| --- | --- | --- | --- |",
    ]
    for row in data["rows"]:
        flat = number(row["flat_ns"])
        naive = number(row["naive_ns"])
        if row["interpretation"] == "below_resolution":
            flat_cell = "**%s ns**" % flat
            naive_cell = "%s ns" % naive
            ratio_cell = "below measurement resolution"
        else:
            flat_cell = "%s ns" % flat
            naive_cell = "%s ns" % naive
            ratio_cell = "**%.1fx faster**" % (row["naive_ns"] / row["flat_ns"])
        lines.append("| %s | %s | %s | %s |" % (row["label"], flat_cell, naive_cell, ratio_cell))
    return newline.join(lines)


def allocation_table(data, newline):
    alloc = data["allocations_per_add"]
    return newline.join(
        [
            "| | Allocations per add |",
            "| --- | --- |",
            "| Flat book | **%s** |" % number(alloc["flat"]),
            "| `std::map` book | **%s** |" % number(alloc["naive"]),
        ]
    )


def replace_region(text, begin, end, table, newline):
    # \r?\n on both sides so a CRLF document matches as readily as an LF one; the
    # replacement is rebuilt with the document's own newline so nothing is converted.
    pattern = re.compile(re.escape(begin) + r"\r?\n.*?\r?\n" + re.escape(end), re.DOTALL)
    replacement = newline.join((begin, table, end))
    new_text, count = pattern.subn(lambda _match: replacement, text)
    if count != 1:
        raise BaselineError(
            "expected exactly one region between %r and %r, found %d" % (begin, end, count)
        )
    return new_text


def render(document_text, data):
    # Preserve the document's existing convention rather than imposing one.
    newline = "\r\n" if "\r\n" in document_text else "\n"
    text = replace_region(
        document_text, BASELINE_BEGIN, BASELINE_END, baseline_table(data, newline), newline
    )
    text = replace_region(
        text, ALLOC_BEGIN, ALLOC_END, allocation_table(data, newline), newline
    )
    return text


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if BENCHMARKS.md differs from the artifact, changing nothing",
    )
    args = parser.parse_args(argv)

    try:
        data = load_artifact(ARTIFACT)
    except BaselineError as error:
        print("baseline artifact is invalid: %s" % error, file=sys.stderr)
        return 2

    # newline="" disables newline translation in both directions, so the document's
    # own line endings survive a rewrite. Without it, writing on Windows converts
    # every LF to CRLF and produces a whole-file diff.
    with open(DOCUMENT, "r", encoding="utf-8", newline="") as handle:
        current = handle.read()
    try:
        rendered = render(current, data)
    except BaselineError as error:
        print("cannot sync %s: %s" % (DOCUMENT.name, error), file=sys.stderr)
        return 2

    if args.check:
        if rendered != current:
            print(
                "%s is out of date with %s. Run scripts/sync_benchmarks.py to update it."
                % (DOCUMENT.name, ARTIFACT.name),
                file=sys.stderr,
            )
            return 1
        print("%s is in sync with %s." % (DOCUMENT.name, ARTIFACT.name))
        return 0

    if rendered != current:
        with open(DOCUMENT, "w", encoding="utf-8", newline="") as handle:
            handle.write(rendered)
        print("updated %s from %s." % (DOCUMENT.name, ARTIFACT.name))
    else:
        print("%s already in sync." % DOCUMENT.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
