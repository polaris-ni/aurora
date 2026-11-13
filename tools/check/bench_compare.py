#!/usr/bin/env python3
"""Baseline store and zero-dependency statistical comparator for bench results.

Two distinct artifacts, deliberately not interchangeable:
  * the checked-in baseline store carries cross-commit trend readings;
  * a same-run two-build A/B comparison carries the regression verdict.

Only an explicit write-baseline action may change the store. A plain compare
never writes it. Every verdict is reproducible: the interleaving order is
derived from an explicit seed and recorded in the report.

Verdicts: pass / regression / insufficient-noise-data / data-missing.
"""
import argparse
import hashlib
import json
import math
import random
import statistics
import sys
import tempfile
from pathlib import Path

SCHEMA_VERSION = 1
MIN_REPETITIONS = 9
DEFAULT_ALPHA = 0.05
DEFAULT_MIN_CHANGE = 0.02
# Above this rank-pair count the exact U distribution costs more than it buys, and the
# normal approximation is accurate anyway.
EXACT_U_LIMIT = 200
VERDICTS = ("pass", "regression", "insufficient-noise-data", "data-missing")


def bucket_key(meta):
    """Bucket identity: cross-bucket readings must never be compared."""
    return "|".join(
        str(meta.get(field, "unknown"))
        for field in ("testbed", "build_type", "backend", "scene")
    )


def _canonical(body):
    return json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _with_checksum(doc):
    body = {key: value for key, value in doc.items() if key != "checksum"}
    doc["checksum"] = hashlib.sha256(_canonical(body).encode("utf-8")).hexdigest()
    return doc


def verify_baseline(path):
    """Return a list of integrity errors; an empty list means the store is intact."""
    file = Path(path)
    if not file.is_file():
        return ["baseline-missing"]
    try:
        doc = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return ["baseline-unparseable"]
    if doc.get("schema_version") != SCHEMA_VERSION:
        return ["baseline-schema-mismatch"]
    body = {key: value for key, value in doc.items() if key != "checksum"}
    expected = hashlib.sha256(_canonical(body).encode("utf-8")).hexdigest()
    if doc.get("checksum") != expected:
        return ["baseline-checksum-mismatch"]
    if not isinstance(doc.get("buckets"), dict):
        return ["baseline-buckets-missing"]
    return []


def collect(directory):
    """Load all result files in a directory. Return (records, errors).

    A skipped, untrusted or incomplete result is a hard data error, never a
    silent pass: the comparator must not turn a missing sample into a green run.
    """
    folder = Path(directory)
    if not folder.is_dir():
        return [], ["results-dir-missing"]
    files = sorted(folder.glob("*.json"))
    if not files:
        return [], ["no-result-files"]
    records = []
    for file in files:
        try:
            doc = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return [], ["unparseable-result: " + file.name]
        if doc.get("schema_version") != SCHEMA_VERSION:
            return [], ["result-schema-mismatch: " + file.name]
        if doc.get("status") != "ok" or doc.get("data_status") != "complete":
            return [], ["skipped-or-untrusted-result: " + file.name]
        meta = doc.get("meta")
        if not isinstance(meta, dict) or not meta.get("tool") or not meta.get("scene"):
            return [], ["incomplete-meta: " + file.name]
        metrics = doc.get("metrics")
        if not isinstance(metrics, dict) or not metrics:
            return [], ["empty-metrics: " + file.name]
        values = {}
        for key, entry in metrics.items():
            if not isinstance(entry, dict) or "value" not in entry:
                return [], ["malformed-metric: " + file.name + ":" + str(key)]
            number = entry["value"]
            if isinstance(number, bool) or not isinstance(number, (int, float)):
                return [], ["non-numeric-metric: " + file.name + ":" + str(key)]
            if not math.isfinite(float(number)):
                return [], ["non-finite-metric: " + file.name + ":" + str(key)]
            values[key] = float(number)
        records.append((bucket_key(meta), values, file.name))
    return records, []


def group_samples(records):
    grouped = {}
    for bucket, values, _name in records:
        grouped.setdefault(bucket, []).append(values)
    return grouped


def _exact_u_distribution(n1, n2):
    """Count every size-n1 subset of the n1+n2 ranks, indexed by its U statistic."""
    total = n1 + n2
    max_u = n1 * n2
    rows = [[0] * (max_u + 1) for _ in range(n1 + 1)]
    rows[0][0] = 1
    for rank in range(1, total + 1):
        for chosen in range(min(rank, n1), 0, -1):
            shift = rank - chosen
            row = rows[chosen]
            previous = rows[chosen - 1]
            for u in range(max_u - shift, -1, -1):
                if previous[u]:
                    row[u + shift] += previous[u]
    return rows[n1]


def _exact_mann_whitney_p(n1, n2, u_observed):
    """Two-sided exact p-value: without it a 9-vs-9 perfect separation cannot survive
    a family of a few hundred metrics, so no regression could ever be declared."""
    counts = _exact_u_distribution(n1, n2)
    tail = sum(count for value, count in enumerate(counts) if value <= u_observed)
    return min(1.0, 2.0 * tail / math.comb(n1 + n2, n1))


def mann_whitney_p(a, b):
    """Two-sided Mann-Whitney U p-value: exact for small tie-free samples, otherwise the
    normal approximation with tie and continuity correction."""
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return 1.0
    combined = sorted([(value, 0) for value in a] + [(value, 1) for value in b],
                      key=lambda item: item[0])
    total = len(combined)
    ranks = [0.0] * total
    tie_groups = []
    i = 0
    while i < total:
        j = i + 1
        while j < total and combined[j][0] == combined[i][0]:
            j += 1
        average = (i + 1 + j) / 2.0
        for k in range(i, j):
            ranks[k] = average
        tie_groups.append(j - i)
        i = j
    rank_sum = sum(ranks[k] for k in range(total) if combined[k][1] == 0)
    u1 = rank_sum - n1 * (n1 + 1) / 2.0
    u = min(u1, n1 * n2 - u1)
    if n1 * n2 <= EXACT_U_LIMIT and all(group == 1 for group in tie_groups):
        return _exact_mann_whitney_p(n1, n2, int(round(u)))
    mean = n1 * n2 / 2.0
    tie_term = sum(t ** 3 - t for t in tie_groups)
    variance = (n1 * n2 / 12.0) * ((total + 1) - tie_term / (total * (total - 1)))
    if variance <= 0.0:
        return 1.0
    z = (u - mean + 0.5) / math.sqrt(variance)
    return min(1.0, max(0.0, math.erfc(abs(z) / math.sqrt(2.0))))


def holm_adjust(pvalues):
    """Holm step-down correction: parallel metrics must not inflate false positives."""
    count = len(pvalues)
    if count == 0:
        return []
    order = sorted(range(count), key=lambda idx: pvalues[idx])
    adjusted = [0.0] * count
    running = 0.0
    for rank, idx in enumerate(order):
        running = max(running, (count - rank) * pvalues[idx])
        adjusted[idx] = min(1.0, running)
    return adjusted


def interleave_sequence(base_count, head_count, seed):
    labels = ["base"] * base_count + ["head"] * head_count
    random.Random(seed).shuffle(labels)
    return labels


def load_budget(path):
    file = Path(path)
    if not file.is_file():
        return None
    try:
        doc = json.loads(file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    metrics = doc.get("metrics")
    return metrics if isinstance(metrics, dict) else None


def run_compare(base_dir, head_dir, seed, budget_path, baseline_path=None,
                min_reps=MIN_REPETITIONS, alpha=DEFAULT_ALPHA,
                min_change=DEFAULT_MIN_CHANGE):
    """Compare two same-run arms. Never mutates the baseline store."""
    report = {
        "schema_version": SCHEMA_VERSION,
        "verdict": "pass",
        "seed": seed,
        "interleave_sequence": [],
        "min_repetitions": min_reps,
        "alpha": alpha,
        "min_change": min_change,
        "buckets": {},
        "metrics": [],
        "errors": [],
    }
    if seed is None:
        report["verdict"] = "data-missing"
        report["errors"].append("missing-interleave-seed")
        return report
    if baseline_path is not None:
        report["errors"].extend(verify_baseline(baseline_path))
    base_records, base_errors = collect(base_dir)
    head_records, head_errors = collect(head_dir)
    report["errors"].extend("base: " + item for item in base_errors)
    report["errors"].extend("head: " + item for item in head_errors)
    if report["errors"]:
        report["verdict"] = "data-missing"
        return report

    base_groups = group_samples(base_records)
    head_groups = group_samples(head_records)
    if set(base_groups) != set(head_groups):
        report["errors"].append("bucket-mismatch: base=" + ",".join(sorted(base_groups))
                                + " head=" + ",".join(sorted(head_groups)))
        report["verdict"] = "data-missing"
        return report
    report["interleave_sequence"] = interleave_sequence(
        len(base_records), len(head_records), seed)
    report["buckets"] = {key: {"base_samples": len(base_groups[key]),
                               "head_samples": len(head_groups[key])}
                         for key in sorted(base_groups)}

    budget = load_budget(budget_path)
    pending = []
    for key in sorted(base_groups):
        base_samples = base_groups[key]
        head_samples = head_groups[key]
        metric_keys = sorted(set().union(*(sample.keys() for sample in base_samples)))
        for metric in metric_keys:
            a = [sample[metric] for sample in base_samples if metric in sample]
            b = [sample[metric] for sample in head_samples if metric in sample]
            entry = {
                "bucket": key,
                "metric": metric,
                "base_samples": len(a),
                "head_samples": len(b),
                "base_median": statistics.median(a) if a else None,
                "head_median": statistics.median(b) if b else None,
            }
            if len(a) < min_reps or len(b) < min_reps or not a or not b:
                entry["verdict"] = "insufficient-noise-data"
                entry["reason"] = "repetition-count-below-minimum"
                report["metrics"].append(entry)
                pending.append(entry)
                continue
            budget_key = key + "|" + metric
            if budget is None or budget_key not in budget:
                entry["verdict"] = "insufficient-noise-data"
                entry["reason"] = "noise-budget-not-calibrated"
                report["metrics"].append(entry)
                pending.append(entry)
                continue
            cv = float(budget[budget_key].get("cv", 0.0))
            base_median = statistics.median(a)
            head_median = statistics.median(b)
            delta = ((head_median - base_median) / abs(base_median)
                     if base_median else head_median - base_median)
            pvalue = mann_whitney_p(a, b)
            entry.update({
                "relative_change": delta,
                "noise_budget": cv,
                "threshold": max(cv, min_change),
                "pvalue_raw": pvalue,
                "exceeds_threshold": delta > max(cv, min_change) and delta > 0.0,
            })
            report["metrics"].append(entry)
            pending.append(entry)

    pvalues = [entry.get("pvalue_raw", 1.0) for entry in pending]
    adjusted = holm_adjust(pvalues)
    for entry, p_adjusted in zip(pending, adjusted):
        if "pvalue_raw" in entry:
            entry["pvalue_adjusted"] = p_adjusted
            if entry["exceeds_threshold"] and p_adjusted < alpha:
                entry["verdict"] = "regression"
            else:
                entry["verdict"] = "pass"
            entry.pop("exceeds_threshold")

    verdicts = [entry["verdict"] for entry in report["metrics"]]
    if "regression" in verdicts:
        report["verdict"] = "regression"
    elif "insufficient-noise-data" in verdicts:
        report["verdict"] = "insufficient-noise-data"
    else:
        report["verdict"] = "pass"
    return report


def run_calibrate(base_dir, head_dir, out_path):
    """Derive per-metric coefficient of variation from a same-code A/A pair."""
    records_a, errors_a = collect(base_dir)
    records_b, errors_b = collect(head_dir)
    if errors_a or errors_b:
        return {"ok": False, "errors": errors_a + errors_b}
    pooled = {}
    for records in (records_a, records_b):
        for bucket, values, _name in records:
            for metric, value in values.items():
                pooled.setdefault(bucket + "|" + metric, []).append(value)
    metrics = {}
    for key in sorted(pooled):
        values = pooled[key]
        if len(values) < 2:
            continue
        mean = statistics.fmean(values)
        deviation = statistics.pstdev(values)
        metrics[key] = {"cv": deviation / abs(mean) if mean else 0.0,
                        "samples": len(values)}
    doc = {"schema_version": SCHEMA_VERSION, "source": "same-code A/A", "metrics": metrics}
    Path(out_path).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return {"ok": True, "metrics": len(metrics)}


def run_write_baseline(results_dir, baseline_path, commit, fingerprint):
    """The only action allowed to mutate the checked-in baseline store."""
    records, errors = collect(results_dir)
    if errors:
        return {"ok": False, "errors": errors}
    buckets = {}
    grouped = group_samples(records)
    for key in sorted(grouped):
        samples = grouped[key]
        metric_keys = sorted(set().union(*(sample.keys() for sample in samples)))
        buckets[key] = {
            "commit": commit,
            "build_fingerprint": fingerprint,
            "samples": len(samples),
            "metrics": {
                metric: {"median": statistics.median(
                    [sample[metric] for sample in samples if metric in sample]),
                    "samples": sum(1 for sample in samples if metric in sample)}
                for metric in metric_keys
            },
        }
    doc = _with_checksum({
        "schema_version": SCHEMA_VERSION,
        "note": "Explicit write only. Plain measurements and comparisons never mutate this file.",
        "buckets": buckets,
    })
    Path(baseline_path).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return {"ok": True, "buckets": len(buckets)}


def _synthetic_result(path, index, values, testbed="win32-release", status="ok", seed=7):
    jitter = random.Random(seed + index)
    doc = {
        "schema_version": SCHEMA_VERSION,
        "status": status,
        "data_status": "complete" if status == "ok" else "data-missing",
        "reason": "" if status == "ok" else "backend-unavailable",
        "meta": {
            "tool": "bench_synthetic",
            "scene": "selftest",
            "testbed": testbed,
            "backend": "headless",
            "build_type": "release",
            "compiler": "clang",
            "commit": "selftest",
            "build_fingerprint": "selftest-fp",
            "warmup_frames": 0,
            "discarded_frames": 0,
            "sampled_frames": 300,
            "repetition": index,
            "interleave_seed": 0,
            "sampling": "steady-clock",
        },
        "metrics": {
            key: {"value": value * jitter.gauss(1.0, 0.01), "unit": "ms"}
            for key, value in values.items()
        },
        "flags": {},
        "scopes": [],
        "trust": {"trustworthy": True, "reason": "all self-checks passed"},
    }
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


def _make_arms(root, count, head_multiplier=1.0, testbed="win32-release",
               skipped_index=None):
    base = Path(root) / "base"
    head = Path(root) / "head"
    base.mkdir(parents=True, exist_ok=True)
    head.mkdir(parents=True, exist_ok=True)
    values = {"app.p99_ms": 4.0, "app.jitter_ms": 0.7}
    for index in range(count):
        _synthetic_result(base / ("run_%02d.json" % index), index, values,
                          testbed=testbed)
        _synthetic_result(head / ("run_%02d.json" % index), index + 100,
                          {key: value * head_multiplier for key, value in values.items()},
                          testbed=testbed,
                          status="skipped" if skipped_index == index else "ok")
    return str(base), str(head)


def run_selftest():
    """Injected mutations: green alone is not evidence, every mutation must turn red."""
    checks = []

    def check(name, condition):
        checks.append((name, bool(condition)))

    with tempfile.TemporaryDirectory() as root:
        # A parallel metric family must not turn one borderline p-value into a
        # regression: replacing the step-down correction with an identity must
        # make these checks fail instead of silently staying green.
        borderline = 0.04
        family_adjusted = holm_adjust([borderline] * 8)
        check("a borderline p is significant on its own",
              holm_adjust([borderline])[0] < DEFAULT_ALPHA)
        check("Holm caps a borderline p by the family size",
              abs(family_adjusted[0] - 8 * borderline) < 1e-9)
        check("parallel metric family does not manufacture significance",
              family_adjusted[0] >= DEFAULT_ALPHA)
        family_pvalues = [0.01, 0.04, 0.03]
        adjusted = holm_adjust(family_pvalues)
        check("Holm never loosens a p-value",
              all(a >= p for p, a in zip(family_pvalues, adjusted)))
        check("Holm step-down matches the three-metric reference",
              all(abs(a - b) < 1e-9 for a, b in zip(adjusted, [0.03, 0.06, 0.06])))

        # A perfect 9-vs-9 separation is the strongest signal nine repetitions can carry.
        # Its exact p-value must still survive a few-hundred-metric family, otherwise the
        # gate can never fire no matter how large the regression is.
        separated_base = [1.0 + 0.001 * i for i in range(MIN_REPETITIONS)]
        separated_head = [2.0 + 0.001 * i for i in range(MIN_REPETITIONS)]
        exact_p = mann_whitney_p(separated_base, separated_head)
        check("perfect separation has a small exact p-value", exact_p < 5.0e-5)
        wide_family = holm_adjust([exact_p] + [1.0] * 246)
        check("perfect separation survives a 247-metric family",
              wide_family[0] < DEFAULT_ALPHA)
        check("overlapping samples keep a large p-value",
              mann_whitney_p([1.0, 2.0, 3.0], [1.0, 2.0, 3.0]) > 0.5)

        base, head = _make_arms(Path(root) / "aa", MIN_REPETITIONS)
        budget = str(Path(root) / "budget.json")
        calibration = run_calibrate(base, head, budget)
        check("A/A calibration succeeds", calibration.get("ok"))

        report = run_compare(base, head, 42, budget)
        check("same-code A/A is pass", report["verdict"] == "pass")
        for repeat in range(2):
            check("same-code A/A stays pass on repeat %d" % (repeat + 1),
                  run_compare(base, head, 42, budget)["verdict"] == "pass")

        replay = run_compare(base, head, 42, budget)
        check("explicit seed is reproducible",
              replay["interleave_sequence"] == report["interleave_sequence"])
        check("explicit seed replay keeps the verdict",
              replay["verdict"] == report["verdict"])
        check("unseeded run is rejected",
              run_compare(base, head, None, budget)["verdict"] == "data-missing")

        regression_base, regression_head = _make_arms(
            Path(root) / "regression", MIN_REPETITIONS, head_multiplier=1.30)
        regression = run_compare(regression_base, regression_head, 42, budget)
        check("injected 30% slowdown is regression", regression["verdict"] == "regression")

        short_base, short_head = _make_arms(Path(root) / "short", 3)
        short = run_compare(short_base, short_head, 42, budget)
        check("too few repetitions is insufficient-noise-data",
              short["verdict"] == "insufficient-noise-data")

        missing_base, missing_head = _make_arms(
            Path(root) / "missing", MIN_REPETITIONS, skipped_index=4)
        missing = run_compare(missing_base, missing_head, 42, budget)
        check("skipped result is data-missing", missing["verdict"] == "data-missing")

        cross_base, cross_head = _make_arms(
            Path(root) / "cross", MIN_REPETITIONS, testbed="other-testbed")
        cross = run_compare(base, cross_head, 42, budget)
        check("cross-testbed comparison is data-missing",
              cross["verdict"] == "data-missing")
        del cross_base

        baseline = str(Path(root) / "baseline.json")
        written = run_write_baseline(base, baseline, "selftest", "selftest-fp")
        check("explicit baseline write succeeds", written.get("ok"))
        check("baseline is intact", verify_baseline(baseline) == [])
        check("intact baseline does not change verdict",
              run_compare(base, head, 42, budget, baseline)["verdict"] == "pass")

        doc = json.loads(Path(baseline).read_text(encoding="utf-8"))
        doc["buckets"].clear()
        Path(baseline).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        check("tampered baseline turns red",
              run_compare(base, head, 42, budget, baseline)["verdict"] == "data-missing")
        Path(baseline).unlink()
        check("missing baseline turns red",
              run_compare(base, head, 42, budget, baseline)["verdict"] == "data-missing")

        plain = run_compare(base, head, 42, budget)
        check("plain compare never writes a baseline",
              not Path(baseline).exists() and plain["verdict"] == "pass")

    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
    failed = [name for name, ok in checks if not ok]
    if failed:
        print("selftest failures: " + ", ".join(failed))
        return 1
    print("bench_compare selftest OK: %d checks" % len(checks))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    compare = sub.add_parser("compare", help="same-run two-build A/B verdict")
    compare.add_argument("--base", required=True)
    compare.add_argument("--head", required=True)
    compare.add_argument("--seed", type=int, default=None)
    compare.add_argument("--noise-budget", required=True)
    compare.add_argument("--baseline", default=None)
    compare.add_argument("--min-repetitions", type=int, default=MIN_REPETITIONS)
    compare.add_argument("--alpha", type=float, default=DEFAULT_ALPHA)
    compare.add_argument("--min-change", type=float, default=DEFAULT_MIN_CHANGE)
    compare.add_argument("--out", default=None)

    calibrate = sub.add_parser("calibrate", help="derive noise budget from an A/A pair")
    calibrate.add_argument("--base", required=True)
    calibrate.add_argument("--head", required=True)
    calibrate.add_argument("--out", required=True)

    write = sub.add_parser("write-baseline", help="explicitly write the baseline store")
    write.add_argument("--results", required=True)
    write.add_argument("--baseline", required=True)
    write.add_argument("--commit", default="unknown")
    write.add_argument("--fingerprint", default="unknown")

    sub.add_parser("selftest", help="injected-mutation self validation")

    args = parser.parse_args()
    if args.command == "selftest":
        return run_selftest()
    if args.command == "calibrate":
        result = run_calibrate(args.base, args.head, args.out)
        print(json.dumps(result, indent=2))
        return 0 if result.get("ok") else 1
    if args.command == "write-baseline":
        result = run_write_baseline(args.results, args.baseline, args.commit, args.fingerprint)
        print(json.dumps(result, indent=2))
        return 0 if result.get("ok") else 1
    report = run_compare(args.base, args.head, args.seed, args.noise_budget,
                         args.baseline, args.min_repetitions, args.alpha, args.min_change)
    text = json.dumps(report, indent=2) + "\n"
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
