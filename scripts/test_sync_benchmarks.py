#!/usr/bin/env python3
"""Regression checks for scripts/sync_benchmarks.py.

Two groups:

  - the validator, which must reject a "faster" row that is not faster, negative
    allocations, a missing noise floor, missing metadata, and empty labels, while
    accepting a well formed artifact.
  - the region renderer, which must be idempotent, preserve the document's own line
    endings (LF or CRLF), leave surrounding bytes untouched, and detect drift.

Standard library only, run with:

    python3 scripts/test_sync_benchmarks.py
"""

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sync_benchmarks as sb  # noqa: E402


def good():
    """A minimal artifact that must validate."""
    return {
        "schema": 1,
        "tool": "ob_baseline_bench",
        "version": "0.1.0",
        "benchmark_command": "./ob_baseline_bench",
        "repetitions": 9,
        "statistic": "median",
        "rows": [
            {"id": "add", "label": "Add", "flat_ns": 30.5, "naive_ns": 84.7, "interpretation": "faster"},
            {"id": "cancel", "label": "Cancel", "flat_ns": 11.5, "naive_ns": 61.1, "interpretation": "faster"},
            {"id": "match", "label": "Match", "flat_ns": 32.2, "naive_ns": 67.4, "interpretation": "faster"},
            {
                "id": "best_bid",
                "label": "Best bid",
                "flat_ns": 0.26,
                "naive_ns": 4.45,
                "interpretation": "below_resolution",
            },
        ],
        "query_noise_floor_ns": 0.17,
        "allocations_per_add": {"flat": 0, "naive": 2.064},
    }


def expect_ok(name, data):
    try:
        sb.validate(data)
    except sb.BaselineError as error:
        raise AssertionError("%s: expected valid, raised %s" % (name, error))


def expect_error(name, mutate):
    data = copy.deepcopy(good())
    mutate(data)
    try:
        sb.validate(data)
    except sb.BaselineError:
        return
    raise AssertionError("%s: expected BaselineError, none raised" % name)


def set_add(data, **fields):
    data["rows"][0].update(fields)


def check(name, condition):
    if not condition:
        raise AssertionError(name)


def validator_cases():
    expect_ok("baseline seed validates", good())

    # The case the review reproduced: 100 ns vs 84.7 ns still marked faster would
    # render "0.8x faster".
    expect_error("faster row with flat above naive", lambda d: set_add(d, flat_ns=100.0))
    expect_error("faster row with flat equal to naive", lambda d: set_add(d, flat_ns=84.7, naive_ns=84.7))

    expect_error("negative flat allocations", lambda d: d["allocations_per_add"].__setitem__("flat", -1))
    expect_error("negative naive allocations", lambda d: d["allocations_per_add"].__setitem__("naive", -0.5))

    expect_error("missing noise floor", lambda d: d.pop("query_noise_floor_ns"))
    expect_error("negative noise floor", lambda d: d.__setitem__("query_noise_floor_ns", -0.1))

    expect_error("wrong schema", lambda d: d.__setitem__("schema", 2))
    expect_error("missing required row", lambda d: d["rows"].pop())
    expect_error("unknown interpretation", lambda d: set_add(d, interpretation="mystery"))
    expect_error("non finite timing", lambda d: set_add(d, flat_ns=float("inf")))

    # Shape and metadata: unknown input must be rejected, not trusted.
    expect_error("missing tool", lambda d: d.pop("tool"))
    expect_error("empty version", lambda d: d.__setitem__("version", ""))
    expect_error("missing benchmark_command", lambda d: d.pop("benchmark_command"))
    expect_error("non integer repetitions", lambda d: d.__setitem__("repetitions", 9.5))
    expect_error("zero repetitions", lambda d: d.__setitem__("repetitions", 0))
    expect_error("empty label", lambda d: set_add(d, label=""))
    expect_error("row not an object", lambda d: d["rows"].__setitem__(0, "add"))
    expect_error("duplicate row id", lambda d: d["rows"].append(copy.deepcopy(d["rows"][0])))

    # Non-dict top level, checked directly since expect_error deep copies a dict.
    for value in (None, [], "x", 3):
        try:
            sb.validate(value)
        except sb.BaselineError:
            continue
        raise AssertionError("top level %r should be rejected" % (value,))


def document(newline):
    """A tiny document carrying both marker regions, joined with the given newline."""
    lines = [
        "# Title",
        "",
        "Intro prose that must survive untouched.",
        "",
        sb.BASELINE_BEGIN,
        "stale baseline",
        sb.BASELINE_END,
        "",
        "Middle prose.",
        "",
        sb.ALLOC_BEGIN,
        "stale allocations",
        sb.ALLOC_END,
        "",
        "Trailing prose.",
        "",
    ]
    return newline.join(lines)


def render_cases():
    data = good()

    for label, newline in (("LF", "\n"), ("CRLF", "\r\n")):
        original = document(newline)
        once = sb.render(original, data)

        # Newline preservation: the rendered document uses only the input's endings.
        if newline == "\r\n":
            check("%s: keeps CRLF" % label, "\r\n" in once)
            check("%s: introduces no bare LF" % label, once.replace("\r\n", "") .find("\n") == -1)
        else:
            check("%s: stays LF" % label, "\r" not in once)

        # Surrounding prose is untouched.
        check("%s: keeps intro" % label, "Intro prose that must survive untouched." in once)
        check("%s: keeps middle" % label, "Middle prose." in once)
        check("%s: keeps trailing" % label, "Trailing prose." in once)

        # The stale placeholder text is gone and the real numbers are in.
        check("%s: replaces baseline placeholder" % label, "stale baseline" not in once)
        check("%s: replaces alloc placeholder" % label, "stale allocations" not in once)
        check("%s: renders a ratio" % label, "2.8x faster" in once)
        check("%s: renders below resolution" % label, "below measurement resolution" in once)
        check("%s: never renders slower" % label, "slower" not in once)

        # Idempotence: rendering an already rendered document changes nothing.
        twice = sb.render(once, data)
        check("%s: render is idempotent" % label, twice == once)

    # A missing marker is an error rather than a silent no-op.
    broken = document("\n").replace(sb.BASELINE_END, "")
    try:
        sb.render(broken, data)
    except sb.BaselineError:
        pass
    else:
        raise AssertionError("missing end marker should raise")

    # Drift: hand editing a generated cell must not survive a re-render.
    drifted = sb.render(document("\n"), data).replace("30.5 ns", "99.9 ns")
    check("drift is detected", sb.render(drifted, data) != drifted)


def main():
    validator_cases()
    render_cases()
    print("all sync_benchmarks checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
