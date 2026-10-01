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
from decimal import Decimal, ROUND_HALF_UP, localcontext
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = REPO_ROOT / "site" / "data" / "bench" / "baseline.json"
DOCUMENT = REPO_ROOT / "BENCHMARKS.md"

REQUIRED_ROW_IDS = ("add", "cancel", "match", "best_bid")

# The single numeric display policy, shared byte-for-byte with site/src/baseline.ts.
# Timings and allocations round to DISPLAY_DECIMALS and then drop trailing zeros;
# ratios render at RATIO_DECIMALS with no trimming. DISPLAY_DECIMALS is 3 because
# that is the smallest precision that reproduces every accepted artifact string
# exactly (the naive allocation figure 2.064 has three fractional digits); a
# smaller cap would silently rewrite a published number. Rounding is decimal
# round-half-up taken from each value's shortest decimal representation, never from
# the binary float, so 30.555 renders "30.555" and a ratio of exactly 1.25 renders
# "1.3" in both languages rather than diverging on IEEE-754 rounding.
DISPLAY_DECIMALS = 3
RATIO_DECIMALS = 1

# Wide enough to quantize the largest finite double (about 309 integer digits) to
# DISPLAY_DECIMALS without the decimal module raising InvalidOperation.
ROUND_CONTEXT_PREC = 400

BASELINE_BEGIN = "<!-- BEGIN GENERATED baseline (scripts/sync_benchmarks.py) -->"
BASELINE_END = "<!-- END GENERATED baseline -->"
ALLOC_BEGIN = "<!-- BEGIN GENERATED allocations (scripts/sync_benchmarks.py) -->"
ALLOC_END = "<!-- END GENERATED allocations -->"


class BaselineError(Exception):
    """Raised when the artifact does not satisfy the schema this script expects."""


def _round_half_up(value, places):
    """Round a finite non-negative number to `places` decimals with decimal
    round-half-up, taken from the value's shortest decimal representation rather
    than the binary float. Returns a fixed-point string with exactly `places`
    fractional digits (no exponent). site/src/baseline.ts implements the same
    routine so the two renderers agree to the byte."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BaselineError("cannot format non-number %r" % (value,))
    if isinstance(value, float) and not math.isfinite(value):
        raise BaselineError("cannot format non-finite value %r" % (value,))
    if value < 0:
        raise BaselineError("cannot format negative value %r" % (value,))
    quantum = Decimal(1).scaleb(-places)
    # A high enough precision that quantizing the largest representable double to
    # `places` decimals cannot raise InvalidOperation. The default 28-digit context
    # cannot hold a ~309-digit coefficient, so a legal but huge timing would fail to
    # render; ROUND_CONTEXT_PREC covers the full finite double range. Decimal(str())
    # is exact regardless of context, so only the quantize needs the wider context.
    with localcontext() as ctx:
        ctx.prec = ROUND_CONTEXT_PREC
        return format(Decimal(str(value)).quantize(quantum, rounding=ROUND_HALF_UP), "f")


def number(value):
    """Format a timing or allocation figure: decimal round-half-up to
    DISPLAY_DECIMALS, then drop trailing zeros and any bare decimal point so whole
    values render without one and the rendered bytes stay stable."""
    text = _round_half_up(value, DISPLAY_DECIMALS)
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text


def ratio(numerator, denominator):
    """Format a "faster" ratio at RATIO_DECIMALS with the shared round-half-up
    policy. The division is IEEE-754, identical to the site, and the rounding is
    taken from that quotient's decimal representation, so an exact 1.25 renders
    "1.3" here and in the site rather than diverging on binary rounding."""
    return _round_half_up(numerator / denominator, RATIO_DECIMALS)


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
    # Reject whitespace-only as well as empty: a metadata field or label that is
    # only spaces would otherwise reach public output as a blank cell.
    if not isinstance(value, str) or not value.strip():
        raise BaselineError("%s must be a non-empty, non-whitespace string" % field)
    return value


def validate(data):
    """Enforce the baseline schema on a parsed object. Kept separate from file I/O
    so the regression tests can exercise it on hand built inputs. Treats the input
    as unknown: a corrupt or truncated file is rejected rather than trusted."""
    if not isinstance(data, dict):
        raise BaselineError("top level must be an object")

    # schema must be the integer 1, not True. In Python True == 1, so a bare
    # inequality would accept schema: true; exclude bool explicitly to stay in
    # step with the site's strict === 1 comparison.
    schema = data.get("schema")
    if isinstance(schema, bool) or schema != 1:
        raise BaselineError("expected schema 1, found %r" % (schema,))

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

    by_id = {}
    for row in rows:
        if not isinstance(row, dict):
            raise BaselineError("every row must be an object")
        row_id = _require_string(row, "id")
        if row_id in by_id:
            raise BaselineError("duplicate row id %r" % row_id)
        by_id[row_id] = row
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
        if interpretation == "faster":
            if row["naive_ns"] <= row["flat_ns"]:
                raise BaselineError(
                    "row %r is marked faster but flat %r is not below naive %r"
                    % (row_id, row["flat_ns"], row["naive_ns"])
                )
            # The derived ratio must itself be finite and positive. Individually
            # finite operands can still overflow to a non-finite quotient (a tiny
            # flat against a huge naive), which would otherwise reach the renderer.
            # Python yields inf here where JavaScript yields Infinity; both are the
            # same rejection.
            derived = row["naive_ns"] / row["flat_ns"]
            if not math.isfinite(derived) or derived <= 0:
                raise BaselineError(
                    "row %r has a non-finite ratio %r" % (row_id, derived)
                )
        # best_bid is an accepted below-resolution result. Pinning its
        # interpretation keeps it from being promoted to a headline ratio without a
        # reviewed schema or measurement change.
        if row_id == "best_bid" and interpretation != "below_resolution":
            raise BaselineError(
                "row 'best_bid' must be below_resolution, found %r" % (interpretation,)
            )

    # Require exactly the supported set: every required id present, and no others,
    # so an unsupported operation cannot silently appear in public output.
    missing = [row_id for row_id in REQUIRED_ROW_IDS if row_id not in by_id]
    if missing:
        raise BaselineError("missing required row ids: %s" % ", ".join(missing))
    unknown = [row_id for row_id in by_id if row_id not in REQUIRED_ROW_IDS]
    if unknown:
        raise BaselineError("unknown row ids: %s" % ", ".join(sorted(unknown)))

    # Normalize to the canonical order so a permuted-but-valid artifact renders
    # identically. REQUIRED_ROW_IDS is the one authoritative order.
    data["rows"] = [by_id[row_id] for row_id in REQUIRED_ROW_IDS]

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
            ratio_cell = "**%sx faster**" % ratio(row["naive_ns"], row["flat_ns"])
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


def detect_newline(document_text):
    """Return the document's single newline convention, or reject a mixed one.

    A document that mixes CRLF and LF (or carries a bare CR) is ambiguous: picking
    one convention would silently rewrite the other inside the generated regions and
    could hide unrelated drift. Fail clearly instead."""
    without_crlf = document_text.replace("\r\n", "")
    has_crlf = "\r\n" in document_text
    if "\r" in without_crlf or (has_crlf and "\n" in without_crlf):
        raise BaselineError("document mixes newline conventions; use only LF or only CRLF")
    return "\r\n" if has_crlf else "\n"


def validate_markers(text):
    """Fail closed on any ambiguous generated-region marker layout before a single
    byte is replaced.

    The regex substitution alone is not enough: a non-greedy begin-to-end match can
    swallow a second begin marker nested inside one region while still reporting one
    match, so a malformed document could have surrounding prose silently discarded.
    This counts each exact token independently, requires exactly one of each, and
    requires the two regions to be well ordered and disjoint (not reversed, nested,
    repeated, or crossed)."""
    markers = (
        ("baseline begin", BASELINE_BEGIN),
        ("baseline end", BASELINE_END),
        ("allocations begin", ALLOC_BEGIN),
        ("allocations end", ALLOC_END),
    )
    offset = {}
    for name, token in markers:
        count = text.count(token)
        if count != 1:
            raise BaselineError("expected exactly one %s marker, found %d" % (name, count))
        offset[token] = text.index(token)

    baseline = (offset[BASELINE_BEGIN], offset[BASELINE_END] + len(BASELINE_END))
    alloc = (offset[ALLOC_BEGIN], offset[ALLOC_END] + len(ALLOC_END))
    if offset[BASELINE_BEGIN] >= offset[BASELINE_END]:
        raise BaselineError("baseline end marker does not follow its begin marker")
    if offset[ALLOC_BEGIN] >= offset[ALLOC_END]:
        raise BaselineError("allocations end marker does not follow its begin marker")
    # Disjoint: one region wholly before the other. Anything else is nested or crossed.
    if not (baseline[1] <= alloc[0] or alloc[1] <= baseline[0]):
        raise BaselineError("baseline and allocations generated regions overlap")


def render(document_text, data):
    # Preserve the document's existing convention rather than imposing one.
    newline = detect_newline(document_text)
    # Reject any ambiguous marker layout before replacing anything, so a malformed
    # document never has part of its prose discarded by a partial substitution.
    validate_markers(document_text)
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
