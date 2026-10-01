#!/usr/bin/env python3
"""Analysis and pass/fail gate for the controlled paired benchmark campaign.

Reads the per-invocation Google Benchmark JSON written by each paired run, forms
a candidate/baseline real-time-median ratio within each matched pair, and reports the
median paired ratio, its dispersion, the absolute medians, and a reproducible
bootstrap 95 percent confidence interval for the median paired ratio.

The gate: the upper confidence bound must be at most 1.05 for every gated flat event
path (always-accepting and bounded add/cancel/match). The noise floor is a drift
control and is reported but not gated. A syntactically complete run is never
discarded after seeing its value; the bootstrap seed is fixed for reproducibility.

    python3 scripts/analyze_ab_v1_bench.py [evidence-dir]
"""

import glob
import json
import os
import random
import statistics
import sys

BOOTSTRAP_SEED = 20260923
BOOTSTRAP_SAMPLES = 20000
GATE = 1.05
GATED = (
    "bm_add_grow",
    "bm_add_bounded",
    "bm_cancel_grow",
    "bm_cancel_bounded",
    "bm_match_grow",
    "bm_match_bounded",
)
DRIFT = ("bm_noise_floor",)


def medians_by_pair(evidence_dir, arm):
    """Return {benchmark_name: {pair_index: real_time_median}}."""
    out = {}
    for path in glob.glob(os.path.join(evidence_dir, "%s_*.json" % arm)):
        pair = int(os.path.basename(path).split("_")[-1].split(".")[0])
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        for entry in data["benchmarks"]:
            name = entry["name"]
            if name.endswith("_median"):
                out.setdefault(name[:-7], {})[pair] = entry["real_time"]
    return out


def bootstrap_median_ci(ratios, rng):
    samples = []
    n = len(ratios)
    for _ in range(BOOTSTRAP_SAMPLES):
        resample = [ratios[rng.randrange(n)] for _ in range(n)]
        samples.append(statistics.median(resample))
    samples.sort()
    lower = samples[int(0.025 * BOOTSTRAP_SAMPLES)]
    upper = samples[int(0.975 * BOOTSTRAP_SAMPLES)]
    return lower, upper


def main(argv):
    evidence_dir = argv[1] if len(argv) > 1 else "bench_results/ab_v1"
    cand = medians_by_pair(evidence_dir, "candidate")
    base = medians_by_pair(evidence_dir, "baseline")
    rng = random.Random(BOOTSTRAP_SEED)

    names = list(GATED) + list(DRIFT)
    print("bootstrap seed %d, %d resamples, gate upper bound <= %.2f" % (
        BOOTSTRAP_SEED, BOOTSTRAP_SAMPLES, GATE))
    print("%-20s %6s %8s %8s %8s %8s %6s" % (
        "benchmark", "pairs", "med", "lo95", "hi95", "cand_ns", "gate"))

    all_pass = True
    for name in names:
        if name not in cand or name not in base:
            print("%-20s  MISSING" % name)
            if name in GATED:
                all_pass = False
            continue
        pairs = sorted(set(cand[name]) & set(base[name]))
        ratios = [cand[name][p] / base[name][p] for p in pairs]
        med = statistics.median(ratios)
        lower, upper = bootstrap_median_ci(ratios, rng)
        cand_ns = statistics.median([cand[name][p] for p in pairs])
        gated = name in GATED
        ok = (upper <= GATE) if gated else True
        if gated and not ok:
            all_pass = False
        print("%-20s %6d %8.3f %8.3f %8.3f %8.2f %6s" % (
            name, len(pairs), med, lower, upper, cand_ns,
            ("PASS" if ok else "FAIL") if gated else "drift"))

    print()
    if all_pass:
        print("GATE PASS: no material regression observed under measured conditions")
        return 0
    print("GATE FAIL or INCONCLUSIVE: at least one gated path exceeds the 1.05 bound")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
