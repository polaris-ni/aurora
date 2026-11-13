#!/usr/bin/env python3
# ============================================================================
# report_upstream.py - weekly upstream version-lag report for vendored deps
# ----------------------------------------------------------------------------
# third_party/ is vendored whole, so no package manager tracks upstream releases.
# This script turns the manual "go check each project's latest version" step into
# a structured report: it reads codespec/third_party_versions.json, asks the
# GitHub REST API for the newest release (or the numerically greatest tag) of each
# upstream, and writes a Markdown table plus a JSON summary.
#
# IMPORTANT: this is an INPUT FOR HUMAN REVIEW, not a pass/fail CVE gate. OSV has
# no C++ package/version mapping for these libraries (queries return zero results
# even for known-vulnerable old versions), and upstream tag naming is inconsistent
# (yotta-*, version-*, start/import, jpeg-*). An auto-red gate here would produce
# false positives. The hard gate is check_third_party_versions.py: it keeps the
# inventory honest against the vendored sources. This report tells a reviewer
# WHICH versions to investigate.
#
# Entries with version_kind == "proxy" are skipped: their version is not an
# upstream release number and comparing it would be meaningless.
#
# Network failures are reported per-row and never silently pass as "up to date".
# Output is all ASCII. Python 3.8+, standard library only.
#
# Usage: report_upstream.py --root <repo> --out-md <path> --out-json <path>
# Env:   GITHUB_TOKEN (optional; raises the unauthenticated rate limit)
# Exit:  0 report written, 2 usage/IO error. Lag alone does NOT change the code.
# ============================================================================
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

INVENTORY_REL = "codespec/third_party_versions.json"
API = "https://api.github.com"
VERSION_RE = re.compile(r"^v?(\d+\.\d+(?:\.\d+)*)$")
NUMERIC_RE = re.compile(r"\d+\.\d+(?:\.\d+)*")
# Pre-release / snapshot tags are never a release target. Upstream tag naming
# is inconsistent, so filter by common markers rather than by one convention.
PRERELEASE_RE = re.compile(r"(?:rc|alpha|beta|dev|pre|snapshot|nightly|unstable)", re.I)


def version_key(v):
    parts = []
    for chunk in v.split("."):
        m = re.match(r"(\d+)", chunk)
        parts.append(int(m.group(1)) if m else 0)
    return tuple(parts)


def get_json(url, token):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "aurora-sca-report",
        **({"Authorization": "Bearer " + token} if token else {}),
    })
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def in_line(version, version_line):
    """True when version belongs to the tracked major/minor line (if declared)."""
    if not version_line:
        return True
    return version == version_line or version.startswith(version_line + ".")


def newest_for(upstream, token, version_line=""):
    """Return (version, source, raw_tag_or_error). Never raises for network errors."""
    try:
        rel = get_json(f"{API}/repos/{upstream}/releases/latest", token)
        tag = str(rel.get("tag_name", ""))
        m = VERSION_RE.match(tag) or re.search(NUMERIC_RE, tag)
        ver = m.group(1) if VERSION_RE.match(tag) else (m.group(0) if m else None)
        if ver and in_line(ver, version_line) and not PRERELEASE_RE.search(tag):
            return (ver, "release", tag)
    except urllib.error.HTTPError as exc:
        if exc.code != 404:
            return (None, "error", f"releases/latest HTTP {exc.code}")
    except Exception as exc:  # noqa: BLE001 - report all network failures as data
        return (None, "error", f"releases/latest: {type(exc).__name__}")

    try:
        tags = get_json(f"{API}/repos/{upstream}/tags?per_page=100", token)
        if not isinstance(tags, list):
            return (None, "error", "tags response is not a list")
        candidates = []
        for t in tags:
            name = str(t.get("name", ""))
            if PRERELEASE_RE.search(name):
                continue
            m = VERSION_RE.match(name)
            if not m:
                # Some projects prefix tags (version-3.54.1, v1.6.0 handled
                # above); accept a trailing numeric version after a separator.
                m2 = re.search(r"(?:^|[/\-_v])(\d+\.\d+(?:\.\d+)*)$", name)
                if not m2:
                    continue
                ver = m2.group(1)
            else:
                ver = m.group(1)
            if not in_line(ver, version_line):
                continue
            candidates.append((version_key(ver), ver, name))
        if not candidates:
            reason = "no stable version tag" + (f" in line {version_line}" if version_line else "")
            return (None, "no-parseable-tags", reason)
        candidates.sort(reverse=True)
        _, ver, raw = candidates[0]
        return (ver, "tags", raw)
    except Exception as exc:  # noqa: BLE001 - report all network failures as data
        return (None, "error", f"tags: {type(exc).__name__}")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--out-md", required=True)
    ap.add_argument("--out-json", required=True)
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    data = json.loads((root / INVENTORY_REL).read_text(encoding="utf-8"))
    entries = data.get("dependencies", [])
    token = os.environ.get("GITHUB_TOKEN", "").strip()

    rows = []
    for e in entries:
        name = e.get("name", "?")
        if e.get("version_kind") == "proxy":
            rows.append({"name": name, "status": "skipped-proxy",
                         "current": e.get("version", ""), "upstream": None,
                         "source": None, "raw": None,
                         "reason": "version is a proxy, not comparable to upstream releases"})
            continue
        upstream = e.get("upstream", "")
        version_line = str(e.get("version_line", "")).strip()
        latest, source, raw = newest_for(upstream, token, version_line)
        cur = str(e.get("version", ""))
        if latest is None:
            status = "unavailable"
        elif version_key(latest) > version_key(cur):
            status = "lag"
        elif version_key(latest) == version_key(cur):
            status = "current"
        else:
            status = "ahead-or-divergent"
        rows.append({"name": name, "status": status, "current": cur,
                     "upstream": latest, "source": source, "raw": raw,
                     "version_line": version_line, "reason": ""})

    lines = [
        "# Vendored dependency upstream version report",
        "",
        "Generated from `codespec/third_party_versions.json`. This is a human-review",
        "input, NOT an automated CVE verdict: OSV has no C++ package/version mapping",
        "for these libraries, and upstream tag naming is inconsistent. Investigate any",
        "`lag` row against upstream release notes before upgrading.",
        "",
        "| Library | Current | Track | Upstream newest | Status | Origin |",
        "|:---|:---|:---|:---|:---|:---|",
    ]
    for r in rows:
        lines.append(
            f"| {r['name']} | {r['current']} | {r.get('version_line') or 'any'} | "
            f"{r['upstream'] or '-'} | {r['status']} | "
            f"{r['source'] or r['reason'] or '-'} |")
    lines += [
        "",
        "`Track` limits the comparison to a major/minor release line (for example",
        "mbedTLS stays on the 3.6 LTS line; the 4.x mainline is a different product",
        "and must not appear as lag). Pre-release tags (rc/alpha/beta/dev) are",
        "excluded. Rows marked `unavailable` mean the upstream API could not be",
        "read; they are NOT evidence that the dependency is current. Rows marked",
        "`skipped-proxy` carry a stand-in version and must be reviewed by hand.",
        "",
    ]

    Path(args.out_md).write_text("\n".join(lines), encoding="utf-8")
    Path(args.out_json).write_text(json.dumps({"dependencies": rows}, indent=2),
                                   encoding="utf-8")

    counts = {}
    for r in rows:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print("[OK] upstream report: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
