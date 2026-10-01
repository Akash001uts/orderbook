#!/usr/bin/env python3
"""Regression checks for scripts/sync_benchmarks.py.

Three groups:

  - display formatting, driven by test/fixtures/benchmarks/display_cases.json so the
    number and ratio renderers agree byte-for-byte with the site's formatters.
  - the validator, driven by test/fixtures/benchmarks/validation_cases.json (plus a
    few in-code non-finite cases JSON cannot encode) so it accepts and rejects the
    same inputs the site's validator does.
  - the region renderer, which must be idempotent, preserve the document's own line
    endings (LF or CRLF) and final-newline state, reject mixed conventions, leave
    surrounding bytes untouched, and detect both document and artifact drift.

Standard library only, run with:

    python3 scripts/test_sync_benchmarks.py
"""

import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sync_benchmarks as sb  # noqa: E402

FIXTURES = sb.REPO_ROOT / "test" / "fixtures" / "benchmarks"


def load_fixture(name):
    with open(FIXTURES / name, "r", encoding="utf-8") as handle:
        return json.load(handle)


def good():
    """The canonical valid artifact, taken from the shared fixture so the Python and
    TypeScript suites start from identical input."""
    return copy.deepcopy(load_fixture("validation_cases.json")["base"])


def check(name, condition):
    if not condition:
        raise AssertionError(name)


def apply_case(base, case):
    """Build a case's artifact from the shared base and its declarative edits. The
    same operations are applied by site/src/baseline.test.ts."""
    if "replace" in case:
        return copy.deepcopy(case["replace"])
    data = copy.deepcopy(base)
    for key, value in case.get("set", {}).items():
        data[key] = value
    for key in case.get("delete", []):
        data.pop(key, None)
    if "rows" in case:
        data["rows"] = copy.deepcopy(case["rows"])
    if "alloc" in case:
        data["allocations_per_add"] = copy.deepcopy(case["alloc"])
    return data


def display_cases():
    fixture = load_fixture("display_cases.json")
    for case in fixture["format_ns"]:
        got = sb.number(case["value"])
        check(
            "format_ns %s: expected %r, got %r" % (case["name"], case["expected"], got),
            got == case["expected"],
        )
    for case in fixture["format_ratio"]:
        got = sb.ratio(case["numerator"], case["denominator"])
        check(
            "format_ratio %s: expected %r, got %r" % (case["name"], case["expected"], got),
            got == case["expected"],
        )


def validator_cases():
    fixture = load_fixture("validation_cases.json")
    base = fixture["base"]
    for case in fixture["cases"]:
        data = apply_case(base, case)
        try:
            sb.validate(data)
            accepted = True
        except sb.BaselineError:
            accepted = False
        check(
            "%s: expected %s" % (case["name"], "valid" if case["valid"] else "invalid"),
            accepted == case["valid"],
        )

    # Non-finite values cannot be encoded in JSON, so they are checked in code.
    def set_add(d, **fields):
        d["rows"][0].update(fields)

    for name, mutate in (
        ("infinite flat timing", lambda d: set_add(d, flat_ns=float("inf"))),
        ("nan flat timing", lambda d: set_add(d, flat_ns=float("nan"))),
        ("infinite noise floor", lambda d: d.__setitem__("query_noise_floor_ns", float("inf"))),
        ("nan naive allocation", lambda d: d["allocations_per_add"].__setitem__("naive", float("nan"))),
    ):
        data = good()
        mutate(data)
        try:
            sb.validate(data)
        except sb.BaselineError:
            continue
        raise AssertionError("%s should be rejected" % name)


def order_cases():
    fixture = load_fixture("validation_cases.json")
    base = fixture["base"]
    for case in fixture["order_cases"]:
        data = copy.deepcopy(base)
        if "rows" in case:
            data["rows"] = copy.deepcopy(case["rows"])
        result = sb.validate(data)
        got = [row["id"] for row in result["rows"]]
        check(
            "order %s: expected %r, got %r" % (case["name"], case["expected_order"], got),
            got == case["expected_order"],
        )


def document(newline, trailing_newline=True):
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
    ]
    text = newline.join(lines)
    if trailing_newline:
        text += newline
    return text


def render_cases():
    data = good()

    for label, newline in (("LF", "\n"), ("CRLF", "\r\n")):
        original = document(newline)
        once = sb.render(original, data)

        # Newline preservation: the rendered document uses only the input's endings.
        if newline == "\r\n":
            check("%s: keeps CRLF" % label, "\r\n" in once)
            check("%s: introduces no bare LF" % label, once.replace("\r\n", "").find("\n") == -1)
        else:
            check("%s: stays LF" % label, "\r" not in once)

        # The document's final-newline state is preserved in both directions.
        check("%s: keeps final newline" % label, once.endswith(newline))
        no_final = document(newline, trailing_newline=False)
        check(
            "%s: keeps absent final newline" % label,
            not sb.render(no_final, data).endswith(newline),
        )

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

    # A mixed-newline document is ambiguous and must be rejected, not silently
    # normalized, so unrelated drift cannot hide inside a generated region.
    mixed = document("\n").replace(sb.BASELINE_END, sb.BASELINE_END + "\r", 1)
    try:
        sb.render(mixed, data)
    except sb.BaselineError:
        pass
    else:
        raise AssertionError("mixed newline document should raise")

    # A missing marker is an error rather than a silent no-op.
    broken = document("\n").replace(sb.BASELINE_END, "")
    try:
        sb.render(broken, data)
    except sb.BaselineError:
        pass
    else:
        raise AssertionError("missing end marker should raise")

    # A duplicated region is ambiguous and must be rejected.
    duplicated = document("\n") + "\n" + document("\n")
    try:
        sb.render(duplicated, data)
    except sb.BaselineError:
        pass
    else:
        raise AssertionError("duplicate markers should raise")

    # A reversed marker pair (END before BEGIN) forms no complete region.
    reversed_markers = "\n".join(
        [sb.BASELINE_END, "body", sb.BASELINE_BEGIN, sb.ALLOC_BEGIN, "a", sb.ALLOC_END]
    )
    try:
        sb.render(reversed_markers, data)
    except sb.BaselineError:
        pass
    else:
        raise AssertionError("reversed markers should raise")

    def expect_render_raises(name, doc):
        try:
            sb.render(doc, data)
        except sb.BaselineError:
            return
        raise AssertionError("%s should raise" % name)

    # A missing begin marker leaves no complete region.
    expect_render_raises("missing begin marker", document("\n").replace(sb.BASELINE_BEGIN, ""))

    # A duplicate begin nested inside an otherwise complete region is exactly what a
    # non-greedy regex would swallow; the independent token count must reject it.
    dup_begin = document("\n").replace(
        sb.BASELINE_BEGIN, sb.BASELINE_BEGIN + "\nstray\n" + sb.BASELINE_BEGIN, 1
    )
    expect_render_raises("duplicate begin marker", dup_begin)

    # A duplicate end marker is equally ambiguous.
    dup_end = document("\n").replace(sb.BASELINE_END, sb.BASELINE_END + "\n" + sb.BASELINE_END, 1)
    expect_render_raises("duplicate end marker", dup_end)

    # The allocation region wholly inside the baseline region: nested, not disjoint.
    nested = "\n".join(
        [
            "intro",
            sb.BASELINE_BEGIN,
            sb.ALLOC_BEGIN,
            "a",
            sb.ALLOC_END,
            sb.BASELINE_END,
            "outro",
        ]
    )
    expect_render_raises("nested regions", nested)

    # Interleaved begin/end from the two regions: crossed, not disjoint.
    crossed = "\n".join(
        [sb.BASELINE_BEGIN, sb.ALLOC_BEGIN, sb.BASELINE_END, sb.ALLOC_END]
    )
    expect_render_raises("crossed regions", crossed)

    # Drift: hand editing a generated cell must not survive a re-render.
    drifted = sb.render(document("\n"), data).replace("30.5 ns", "99.9 ns")
    check("document drift is detected", sb.render(drifted, data) != drifted)

    # Artifact drift: changing a source value must change the rendered document, so
    # --check catches a stale table when the artifact moves.
    rendered = sb.render(document("\n"), data)
    moved = good()
    moved["rows"][0]["flat_ns"] = 31.9
    check("artifact drift is detected", sb.render(document("\n"), moved) != rendered)


def main():
    display_cases()
    validator_cases()
    order_cases()
    render_cases()
    print("all sync_benchmarks checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
