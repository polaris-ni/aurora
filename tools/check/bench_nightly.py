#!/usr/bin/env python3
"""Nightly same-runner A/B benchmark driver.

The job builds a base checkout and a head checkout on the same runner, samples
both arms in a seeded interleaved order, calibrates the noise budget with a
same-code A/A pair, and emits a machine-readable verdict plus a job summary.

This is an observability job, not a gate. The four comparator verdicts
(pass / regression / insufficient-noise-data / data-missing) never change the
job exit status; only an infrastructure failure (missing binaries, no result
files at all) does. A skipped or untrusted run is recorded with its reason code
and reported as missing coverage, never folded into a green reading.

Zero third-party dependencies: reuses bench_compare.py in the same directory.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import bench_compare as bc

TOOLS = (
    "bench_render",
    "bench_scroll",
    "bench_theme",
    "bench_gpu",
    "bench_win32_present",
    "bench_idle_cpu",
)

# Only the scroll harness takes a CLI; the other tools run their fixed scenes.
TOOL_ARGS = {
    "bench_scroll": ["--scene", "all", "--frames", "240", "--warmup", "20", "--format", "json"],
}

CAMPAIGNS = ("calibration", "comparison")


def result_doc(status="ok", data_status="complete", reason="", tool="bench_fake",
               scene="default", value=10.0, metric="frame_p99"):
    """Build one synthetic result document for the self-validation."""
    meta = {
        "tool": tool,
        "scene": scene,
        "testbed": "selftest",
        "backend": "headless",
        "build_type": "release",
        "compiler": "clang",
        "commit": "unknown",
        "build_fingerprint": "unknown",
        "warmup_frames": 0,
        "discarded_frames": 0,
        "sampled_frames": 1,
        "repetition": 0,
        "interleave_seed": 0,
        "sampling": "steady-clock",
    }
    return {
        "schema_version": bc.SCHEMA_VERSION,
        "status": status,
        "data_status": data_status,
        "reason": reason,
        "meta": meta,
        "metrics": {metric: {"value": value, "unit": "ms"}},
        "flags": {},
        "scopes": [],
        "trust": {"trustworthy": status == "ok", "reason": reason},
    }


def classify_results(raw_dir, flat_dir):
    """Split one campaign arm into usable readings and explicit missing coverage.

    Usable files are copied into a flat directory with a repetition-prefixed
    name so the comparator sees every repetition as a separate sample. Skipped,
    untrusted or malformed files are NEVER copied: they are reported instead of
    being silently turned into a green verdict.
    """
    flat = Path(flat_dir)
    shutil.rmtree(flat, ignore_errors=True)
    flat.mkdir(parents=True, exist_ok=True)
    skipped = []
    usable = 0
    for rep_dir in sorted(Path(raw_dir).glob("rep-*")):
        for file in sorted(rep_dir.glob("*.json")):
            try:
                doc = json.loads(file.read_text(encoding="utf-8"))
            except (OSError, ValueError) as err:
                skipped.append({"file": file.name, "reason": "unparseable:" + str(err)})
                continue
            if doc.get("status") != "ok" or doc.get("data_status") != "complete":
                skipped.append({"file": file.name,
                                "tool": doc.get("meta", {}).get("tool", "unknown"),
                                "scene": doc.get("meta", {}).get("scene", "unknown"),
                                "reason": doc.get("reason") or doc.get("status", "unknown")})
                continue
            shutil.copy2(file, flat / (rep_dir.name + "-" + file.name))
            usable += 1
    return usable, skipped


def find_binary(bin_dir, tool):
    """Locate one bench executable; MSVC puts it in a config subdirectory."""
    root = Path(bin_dir)
    candidates = [root / tool, root / (tool + ".exe")]
    for config in ("Release", "Debug", "RelWithDebInfo"):
        candidates.append(root / config / tool)
        candidates.append(root / config / (tool + ".exe"))
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


def resolve_arm(bin_dir, label):
    """Return (binaries, missing) for one arm."""
    binaries = {}
    missing = []
    for tool in TOOLS:
        exe = find_binary(bin_dir, tool)
        if exe is None:
            missing.append(label + ":missing-binary:" + tool)
        else:
            binaries[tool] = exe
    return binaries, missing


def sample_campaign(campaign, arm_bins, work, repetitions, seed, testbed, commits):
    """Run one seeded interleaved campaign. Return (usable counts, skipped, errors)."""
    order = bc.interleave_sequence(repetitions, repetitions, seed)
    counters = {"base": 0, "head": 0}
    errors = []
    for label in order:
        counters[label] += 1
        rep = counters[label]
        out_dir = Path(work) / campaign / label / ("rep-%02d" % rep)
        out_dir.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env.update({
            "AURORA_BENCH_RESULT_DIR": str(out_dir),
            "AURORA_BENCH_REPETITION": str(rep),
            "AURORA_BENCH_INTERLEAVE_SEED": str(seed),
            "AURORA_BENCH_TESTBED": testbed,
            "AURORA_BENCH_COMMIT": commits[label],
            "AURORA_BENCH_BUILD_FINGERPRINT": campaign + "-" + label,
        })
        for tool in TOOLS:
            exe = arm_bins[label].get(tool)
            if exe is None:
                continue
            argv = [exe] + TOOL_ARGS.get(tool, [])
            try:
                subprocess.run(argv, env=env, stdin=subprocess.DEVNULL,
                               capture_output=True, text=True, timeout=900, check=False)
            except (OSError, subprocess.TimeoutExpired) as err:
                errors.append(campaign + "/" + label + "/rep-%02d/" % rep + tool
                              + ":run-failed:" + type(err).__name__)
    usable = {}
    skipped = {}
    for label in ("base", "head"):
        raw_dir = Path(work) / campaign / label
        flat_dir = Path(work) / campaign / (label + "-flat")
        count, missing = classify_results(raw_dir, flat_dir)
        usable[label] = count
        skipped[label] = missing
    return usable, skipped, errors


def build_summary(report, extra):
    """Render the job summary. Markdown only, no terminal control characters."""
    lines = [
        "# Nightly benchmark verdict",
        "",
        "- verdict: **%s**" % report["verdict"],
        "- testbed: `%s`" % extra["testbed"],
        "- seed: `%d`, repetitions per arm: `%d`" % (extra["seed"], extra["repetitions"]),
        "- base commit: `%s`, head commit: `%s`" % (extra["base_commit"], extra["head_commit"]),
        "- calibration metrics: `%d`" % extra["calibration_metrics"],
        "",
        "The nightly job is observational. A regression emits a warning only; "
        "pull request status and the existing gate matrix are unchanged.",
        "",
    ]
    if extra["skipped"]:
        lines += ["## Missing coverage (reported, never counted as a pass)", ""]
        for label in ("base", "head"):
            for item in extra["skipped"].get(label, []):
                lines.append("- %s: `%s` (%s / %s) -- `%s`" % (
                    label, item["file"], item.get("tool", "unknown"),
                    item.get("scene", "unknown"), item["reason"]))
        lines.append("")
    if extra["errors"]:
        lines += ["## Infrastructure errors", ""]
        lines += ["- " + item for item in extra["errors"]]
        lines.append("")
    regressions = [entry for entry in report.get("metrics", [])
                   if entry.get("verdict") == "regression"]
    if regressions:
        lines += ["## Regressions", ""]
        for entry in regressions[:20]:
            lines.append("- `%s` / `%s`: %.1f%% (p_adj=%.4f)" % (
                entry["bucket"], entry["metric"],
                100.0 * entry.get("relative_change", 0.0),
                entry.get("pvalue_adjusted", 1.0)))
        lines.append("")
    if report.get("errors"):
        lines += ["## Comparator errors", ""]
        lines += ["- " + item for item in report["errors"]]
        lines.append("")
    lines += ["## Machine-readable report", "", "```json",
              json.dumps({"verdict": report["verdict"], "seed": report["seed"],
                          "buckets": report["buckets"]}, indent=2),
              "```", ""]
    return "\n".join(lines)


def run(args):
    """Execute both campaigns and produce report + summary."""
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    base_bins, base_missing = resolve_arm(args.base_bin, "base")
    head_bins, head_missing = resolve_arm(args.head_bin, "head")
    infra = base_missing + head_missing
    if infra:
        print("::error::nightly benchmark infrastructure incomplete: " + ", ".join(infra))
        return 1

    commits = {"base": args.base_commit, "head": args.head_commit}
    # Calibration is a same-code A/A pair: both arms use the head binaries.
    calibration_bins = {"base": head_bins, "head": head_bins}
    comparison_bins = {"base": base_bins, "head": head_bins}

    cal_usable, _cal_skipped, cal_errors = sample_campaign(
        "calibration", calibration_bins, work, args.repetitions, args.seed,
        args.testbed, {"base": args.head_commit, "head": args.head_commit})
    budget_path = work / "noise-budget.json"
    calibration = bc.run_calibrate(str(work / "calibration" / "base-flat"),
                                   str(work / "calibration" / "head-flat"),
                                   str(budget_path))

    cmp_usable, skipped, cmp_errors = sample_campaign(
        "comparison", comparison_bins, work, args.repetitions, args.seed,
        args.testbed, commits)

    report = bc.run_compare(str(work / "comparison" / "base-flat"),
                            str(work / "comparison" / "head-flat"),
                            args.seed, str(budget_path),
                            baseline_path=args.baseline)
    report["campaigns"] = {
        "calibration": {"usable_files": cal_usable, "errors": cal_errors,
                        "ok": calibration.get("ok", False)},
        "comparison": {"usable_files": cmp_usable, "errors": cmp_errors},
    }
    report["missing_coverage"] = skipped
    report["testbed"] = args.testbed

    extra = {
        "testbed": args.testbed,
        "seed": args.seed,
        "repetitions": args.repetitions,
        "base_commit": args.base_commit,
        "head_commit": args.head_commit,
        "calibration_metrics": calibration.get("metrics", 0),
        "skipped": skipped,
        "errors": infra + cal_errors + cmp_errors,
    }
    Path(args.report).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    Path(args.summary).write_text(build_summary(report, extra), encoding="utf-8")

    if report["verdict"] == "regression":
        print("::warning::nightly benchmark regression detected; see the job summary")
    elif report["verdict"] != "pass":
        print("::warning::nightly benchmark verdict " + report["verdict"]
              + "; the reading is not a clean pass")
    for label in ("base", "head"):
        if skipped.get(label):
            print("::notice::%s has %d skipped/untrusted result file(s); "
                  "reported as missing coverage" % (label, len(skipped[label])))
    if infra or cal_errors or cmp_errors:
        print("::error::nightly benchmark infrastructure errors: "
              + ", ".join(extra["errors"]))
        return 1
    return 0


def selftest(_args):
    """Injected-mutation self-validation. Green alone is not evidence."""
    with tempfile.TemporaryDirectory(prefix="aurora-nightly-") as tmp:
        root = Path(tmp)
        failures = []
        # Deterministic interleaving for an explicit seed.
        if bc.interleave_sequence(9, 9, 4711) != bc.interleave_sequence(9, 9, 4711):
            failures.append("interleave-not-reproducible")
        if bc.interleave_sequence(9, 9, 4711) == bc.interleave_sequence(9, 9, 4712):
            failures.append("seed-not-effective")

        # A clean A/A pair plus a synthetic noise budget must not report a regression.
        for label in ("base", "head"):
            arm = root / label
            arm.mkdir()
            for rep in range(9):
                doc = result_doc(value=10.0 + rep * 0.01)
                (arm / ("r%02d.json" % rep)).write_text(json.dumps(doc), encoding="utf-8")
        budget = root / "budget.json"
        budget.write_text(json.dumps({
            "schema_version": bc.SCHEMA_VERSION,
            "metrics": {"selftest|release|headless|default|frame_p99": {"cv": 0.01, "samples": 18}},
        }), encoding="utf-8")
        clean = bc.run_compare(str(root / "base"), str(root / "head"), 4711, str(budget))
        if clean["verdict"] != "pass":
            failures.append("clean-pair-not-pass:" + clean["verdict"])

        # Mutation: a 30 percent slowdown on one arm must turn the verdict red.
        for rep in range(9):
            doc = result_doc(value=13.0 + rep * 0.01)
            (root / "head" / ("r%02d.json" % rep)).write_text(json.dumps(doc), encoding="utf-8")
        mutated = bc.run_compare(str(root / "base"), str(root / "head"), 4711, str(budget))
        if mutated["verdict"] != "regression":
            failures.append("injected-regression-not-detected:" + mutated["verdict"])
        # Restore must go green again.
        for rep in range(9):
            doc = result_doc(value=10.0 + rep * 0.01)
            (root / "head" / ("r%02d.json" % rep)).write_text(json.dumps(doc), encoding="utf-8")
        restored = bc.run_compare(str(root / "base"), str(root / "head"), 4711, str(budget))
        if restored["verdict"] != "pass":
            failures.append("restore-not-green:" + restored["verdict"])

        # A skipped result must be separated out with its reason code, never copied.
        raw = root / "raw"
        (raw / "rep-01").mkdir(parents=True)
        (raw / "rep-01" / "ok.json").write_text(json.dumps(result_doc()), encoding="utf-8")
        (raw / "rep-01" / "skipped.json").write_text(
            json.dumps(result_doc(status="skipped", data_status="data-missing",
                                  reason="backend-unavailable", tool="bench_win32_present")),
            encoding="utf-8")
        usable, skipped = classify_results(raw, root / "flat")
        if usable != 1 or len(skipped) != 1 or skipped[0]["reason"] != "backend-unavailable":
            failures.append("skipped-result-misclassified")
        if list((root / "flat").glob("*.json")) != [root / "flat" / "rep-01-ok.json"]:
            failures.append("skipped-result-leaked-into-comparison")

        # Missing seed is a hard data-missing, never a pass.
        no_seed = bc.run_compare(str(root / "base"), str(root / "head"), None, str(budget))
        if no_seed["verdict"] != "data-missing":
            failures.append("missing-seed-not-rejected")

        if failures:
            for item in failures:
                print("[FAIL] " + item)
            return 1
        print("[OK] nightly benchmark driver self-validation: 8 assertions passed")
        return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    runner = sub.add_parser("run", help="execute calibration and A/B campaigns")
    runner.add_argument("--base-bin", required=True)
    runner.add_argument("--head-bin", required=True)
    runner.add_argument("--work", required=True)
    runner.add_argument("--report", required=True)
    runner.add_argument("--summary", required=True)
    runner.add_argument("--baseline", default=None)
    runner.add_argument("--testbed", required=True)
    runner.add_argument("--base-commit", default="unknown")
    runner.add_argument("--head-commit", default="unknown")
    runner.add_argument("--seed", type=int, required=True)
    runner.add_argument("--repetitions", type=int, default=bc.MIN_REPETITIONS)
    runner.set_defaults(func=run)
    check = sub.add_parser("selftest", help="injected-mutation self validation")
    check.set_defaults(func=selftest)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
