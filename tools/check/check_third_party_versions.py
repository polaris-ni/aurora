#!/usr/bin/env python3
# ============================================================================
# check_third_party_versions.py - vendored dependency version inventory gate
# ----------------------------------------------------------------------------
# Why a gate: third_party/ is vendored as whole source trees with no package
# manifest (.gitmodules / vcpkg / conan / package.json), so no package manager
# or dependabot ecosystem can see these C++ dependencies. The version ledger was
# maintained by hand and drifted: THIRD_PARTY_LICENSES.md was missing entries
# and one entry pointed at a CMake mirror instead of the vendored source. A CVE
# review then had no machine-readable list of what is actually shipped.
#
# This gate makes codespec/third_party_versions.json the single source of truth
# and verifies it against the vendored source on every ctest run:
#   1) [blocking] inventory is missing / unparseable / has zero entries
#   2) [blocking] duplicate names, or a name/source/pattern/version field empty
#   3) [blocking] a declared source file does not exist, or its regex does not
#      match exactly once (a silent no-match must never pass)
#   4) [blocking] the version parsed from the source differs from the declared
#      version (the drift this gate exists to catch)
#   5) [blocking] coverage: every top-level entry under third_party/ must be
#      represented in the inventory, and vice versa. A newly vendored library
#      that nobody added to the inventory fails here.
#   6) [blocking] the libjpeg-turbo CMake mirror (cmake/AuroraImageCodecs.cmake)
#      must agree with the vendored source version.
#
# Python 3.8+, standard library only. Output is all ASCII.
#
# Usage: check_third_party_versions.py [--root <aurora_root>]
# Exit:  0 clean, 1 findings, 2 usage/IO error.
# ============================================================================
import argparse
import json
import os
import re
import sys
from pathlib import Path

INVENTORY_REL = "codespec/third_party_versions.json"
THIRD_PARTY_REL = "third_party"
CMAKE_MIRROR_REL = "cmake/AuroraImageCodecs.cmake"
CMAKE_MIRROR_PATTERN = re.compile(r'set\(VERSION\s+"([0-9.]+)"\)')

# A top-level directory maps to itself; a single header file maps to its stem.
# This mirrors how the inventory names stb_image.h as "stb_image".
HEADER_SUFFIXES = (".h", ".hpp")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        return d
    return d


def top_level_names(root):
    """Return the set of inventory names implied by third_party/ contents."""
    base = root / THIRD_PARTY_REL
    if not base.is_dir():
        return set(), ["missing third_party/ directory"]
    names = set()
    for entry in base.iterdir():
        if entry.is_dir():
            names.add(entry.name)
        elif entry.is_file() and entry.name.endswith(HEADER_SUFFIXES):
            names.add(entry.name.rsplit(".", 1)[0])
    return names, []


def parse_version(entry, text, rel_path, problems):
    """Parse the version out of a vendored source file using its declared rule."""
    pattern = entry.get("pattern", "")
    try:
        rx = re.compile(pattern)
    except re.error as exc:
        problems.append(f"{entry.get('name', '?')}: invalid regex: {exc}")
        return None

    hits = rx.findall(text)
    if len(hits) == 0:
        problems.append(f"{entry.get('name', '?')}: pattern matched 0 times in {rel_path}")
        return None
    if len(hits) > 1 and not entry.get("compose"):
        problems.append(
            f"{entry.get('name', '?')}: pattern matched {len(hits)} times in {rel_path}; "
            "expected exactly one")
        return None

    compose = entry.get("compose")
    if compose:
        # e.g. "major.minor.patch" over hits of (MACRO_SUFFIX, value) pairs.
        if not all(isinstance(h, tuple) and len(h) == 2 for h in hits):
            problems.append(f"{entry.get('name', '?')}: compose expects (key, value) capture pairs")
            return None
        parts = {k.lower(): v for k, v in hits}
        keys = compose.split(".")
        missing = [k for k in keys if k not in parts]
        if missing:
            problems.append(f"{entry.get('name', '?')}: compose keys missing from source: {missing}")
            return None
        return ".".join(parts[k] for k in keys)

    hit = hits[0]
    if isinstance(hit, tuple):
        hit = next((x for x in hit if x), "")
    return str(hit)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="aurora repository root")
    args = ap.parse_args(argv)

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[2]
    inv_path = root / INVENTORY_REL
    if not inv_path.is_file():
        print(f"[FAIL] inventory missing: {INVENTORY_REL}")
        return 1

    try:
        data = json.loads(inv_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"[FAIL] inventory unreadable: {exc}")
        return 1

    entries = data.get("dependencies")
    if not isinstance(entries, list) or len(entries) == 0:
        print("[FAIL] inventory has zero dependency entries")
        return 1

    problems = []
    seen_names = set()
    covered = set()

    for i, e in enumerate(entries):
        if not isinstance(e, dict):
            problems.append(f"entry #{i}: not an object")
            continue
        name = str(e.get("name", "")).strip()
        if not name:
            problems.append(f"entry #{i}: empty name")
            continue
        if name in seen_names:
            problems.append(f"{name}: duplicate entry")
            continue
        seen_names.add(name)

        for field in ("version", "source", "pattern", "version_kind", "upstream"):
            if not str(e.get(field, "")).strip():
                problems.append(f"{name}: empty or missing field '{field}'")

        rel = str(e.get("source", ""))
        src = root / rel
        if not src.is_file():
            problems.append(f"{name}: source file not found: {rel}")
            continue
        # Coverage is derived from the source path, not from the name, so a
        # renamed entry cannot silently drop a library. source is
        # "third_party/<lib>/..." or "third_party/<lib>.h".
        parts = rel.replace("\\", "/").split("/")
        if len(parts) < 2:
            problems.append(f"{name}: source path has no library segment: {rel}")
            continue
        lib_segment = parts[1]
        if any(lib_segment.endswith(sfx) for sfx in HEADER_SUFFIXES):
            lib_segment = lib_segment.rsplit(".", 1)[0]
        covered.add(lib_segment)

        try:
            text = src.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            problems.append(f"{name}: cannot read {rel}: {exc}")
            continue

        parsed = parse_version(e, text, rel, problems)
        if parsed is None:
            continue
        declared = str(e.get("version", "")).strip()
        if parsed != declared:
            problems.append(
                f"{name}: version drift: inventory says '{declared}', source says '{parsed}' ({rel})")

    # --- coverage guard: inventory <-> third_party/ top level ---
    top, top_problems = top_level_names(root)
    problems.extend(top_problems)
    if top:
        uncovered = sorted(top - covered)
        stale = sorted(covered - top)
        for n in uncovered:
            problems.append(f"coverage: '{n}' exists under third_party/ but is absent from inventory")
        for n in stale:
            problems.append(f"coverage: '{n}' is in inventory but has no third_party/ entry")
        if not top:
            problems.append("coverage: third_party/ yielded zero top-level entries")

    # --- CMake mirror cross-check (libjpeg-turbo) ---
    mirror = root / CMAKE_MIRROR_REL
    jpeg = next((e for e in entries if isinstance(e, dict) and e.get("name") == "libjpeg-turbo"), None)
    if jpeg:
        if not mirror.is_file():
            problems.append(f"libjpeg-turbo: mirror not found: {CMAKE_MIRROR_REL}")
        else:
            mtext = mirror.read_text(encoding="utf-8", errors="replace")
            mhits = CMAKE_MIRROR_PATTERN.findall(mtext)
            if len(mhits) != 1:
                problems.append(
                    f"libjpeg-turbo: mirror VERSION matched {len(mhits)} times in {CMAKE_MIRROR_REL}; expected 1")
            elif mhits[0] != str(jpeg.get("version", "")).strip():
                problems.append(
                    f"libjpeg-turbo: mirror says '{mhits[0]}', inventory says '{jpeg.get('version')}'")

    if problems:
        for p in problems:
            print(f"[FAIL] {p}")
        print(f"[FAIL] {len(problems)} finding(s)")
        return 1

    print(f"[OK] {len(seen_names)} vendored dependency versions match their sources "
          f"and cover all {len(top)} third_party/ entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
