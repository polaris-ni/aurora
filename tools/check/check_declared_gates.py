#!/usr/bin/env python3
# ============================================================================
# check_declared_gates.py - "declared active gate must really be registered"
# ----------------------------------------------------------------------------
# Closes the gap between documentation and CTest: docs can claim a gate is a
# one-vote-veto check while no file, no case and no add_test() exists for it.
# A full audit found 21 unregistered names, of which 20 are examples / retired
# references / planned files / regex false positives, so this checker does NOT
# scan docs for names. It uses an explicitly reviewed declaration list plus a
# reason-bearing exemption list, both next to this file.
#
# Registration truth source: <build-dir>/CTestTestfile.cmake (and its subdirs),
# i.e. exactly what CMake emitted for add_test(). Sharded runner names
# (<stem>_s<N>) also satisfy the declaration for <stem>.
#
# Hard-failure guards (ARCHITECTURE.md 14.6 "0 hits => hard failure"):
#   * declaration list parses to zero names
#   * registration set parses to zero names
#   * any declared name is missing from the registration set
#   * a name appears in both the declaration list and the exemption list
# Stale exemptions (a name that became registered) are reported as notes only.
#
# Usage: check_declared_gates.py <build-dir>
# Exit: 0 clean, 1 findings, 2 usage/IO error. Output is all ASCII.
# ============================================================================
import re
import sys
from pathlib import Path

CHECK_DIR = Path(__file__).resolve().parent
DECLARED_FILE = CHECK_DIR / "declared_gates.txt"
EXEMPTIONS_FILE = CHECK_DIR / "declared_gates_exemptions.txt"
TESTFILE_NAME = "CTestTestfile.cmake"

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ADD_TEST_RE = re.compile(r'^\s*add_test\(\s*"([^"]+)"')
SUBDIR_RE = re.compile(r'"([^"]+)"')
SHARD_RE = re.compile(r"_s\d+$")


def read_names(path):
    """Return (names, problems) from a plain `name` or `name | reason` list."""
    if not path.is_file():
        return [], [f"missing list file: {path.name}"]
    names = []
    problems = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name = line.split("|", 1)[0].strip()
        reason = line.split("|", 1)[1].strip() if "|" in line else ""
        if not NAME_RE.match(name):
            problems.append(f"{path.name}:{number}: invalid name '{name}'")
            continue
        if path == EXEMPTIONS_FILE and not reason:
            problems.append(f"{path.name}:{number}: exemption '{name}' has no reason")
            continue
        if name in names:
            problems.append(f"{path.name}:{number}: duplicate name '{name}'")
            continue
        names.append(name)
    return names, problems


def registered_names(root):
    """Collect every add_test() name from the CTest testfiles of this build."""
    names = set()
    visited = set()
    pending = [root / TESTFILE_NAME]
    while pending:
        path = pending.pop()
        key = str(path.resolve())
        if key in visited or not path.is_file():
            continue
        visited.add(key)
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            matched = ADD_TEST_RE.match(line)
            if matched:
                names.add(matched.group(1))
            elif line.lstrip().startswith("subdirs("):
                for sub in SUBDIR_RE.findall(line):
                    pending.append(path.parent / sub / TESTFILE_NAME)
    return names


def is_registered(name, registered):
    if name in registered:
        return True
    # Sharded runners register "<stem>_s<k>" instead of "<stem>".
    return any(SHARD_RE.sub("", candidate) == name for candidate in registered)


def main(argv):
    if len(argv) != 2:
        print("[FAIL] usage: check_declared_gates.py <build-dir>")
        return 2
    build = Path(argv[1])
    if not build.is_dir():
        print(f"[FAIL] build directory does not exist: {build}")
        return 2

    declared, declared_problems = read_names(DECLARED_FILE)
    exempt, exempt_problems = read_names(EXEMPTIONS_FILE)
    registered = registered_names(build)
    problems = declared_problems + exempt_problems

    if not declared:
        problems.append("declared gate list is empty: a 0-name list must never pass")
    if not registered:
        problems.append("parsed 0 registered CTest names: registration source is missing or unparseable")

    for name in declared:
        if name in exempt:
            problems.append(f"'{name}' is both declared and exempt")
        elif registered and not is_registered(name, registered):
            problems.append(f"declared active gate '{name}' is not registered in this build")

    for name in exempt:
        if name in registered:
            print(f"[note] exemption '{name}' is now registered; remove it from the exemption list")

    if problems:
        for problem in problems:
            print(f"[FAIL] {problem}")
        print(f"check_declared_gates: {len(problems)} finding(s), "
              f"declared={len(declared)} exempt={len(exempt)} registered={len(registered)}")
        return 1
    print(f"[OK] {len(declared)} declared active gates all registered "
          f"(registration set: {len(registered)} names, {len(exempt)} reviewed exemptions)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
