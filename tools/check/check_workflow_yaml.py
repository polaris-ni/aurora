#!/usr/bin/env python3
# ============================================================================
# check_workflow_yaml.py - guard GitHub Actions YAML scalar hazards
# ----------------------------------------------------------------------------
# ci.yml documents (near its env: block) a trap that had no enforcement: a `#`
# line inside a FOLDED (> / >-) scalar under `env:` is CONTENT, not a YAML
# comment. Such a scalar is expanded through ${{ env.* }} into a shell command;
# the `#` then comments out the rest of that line and its trailing `\`
# continuation, silently dropping arguments or turning the next line into a
# stray command (exit 127). The rule lived only in a comment, so nothing stopped
# it from regressing.
#
# This gate makes it mechanical, WITHOUT false positives on legitimate shell
# comments: a `#` line inside a `run: |` LITERAL block is passed to bash as a
# normal shell comment and is explicitly allowed. Only folded scalars under an
# `env:` mapping are hazardous, because folding joins them into one shell line.
#
# Rules:
#   1) [blocking] structural sanity that needs NO third-party module: workflow
#      files must declare top-level `on:` and `jobs:` keys, must not use tab
#      indentation, and must not repeat a top-level key. dependabot.yml must
#      declare `version:` and `updates:`. When PyYAML is available, an extra
#      yaml.safe_load parse is applied; when it is not, a note is printed and
#      these dependency-free checks still run (so the gate is never inert).
#   2) [blocking] a `#` line inside a folded (> / >-) scalar whose key lives
#      under an `env:` mapping. Comment lines outside block scalars are fine.
#   3) [blocking] any `env:` scalar VALUE containing a `#` character (the
#      original failure mode).
#   4) [blocking] zero workflow files found, or zero env scalars inspected, is
#      a hard failure (ARCHITECTURE.md 14.6 "0 hits => hard failure").
#
# Output is all ASCII. Python 3.8+, standard library only.
#
# Usage: check_workflow_yaml.py [--root <aurora_root>]
# Exit:  0 clean, 1 findings, 2 usage/IO error.
# ============================================================================
import argparse
import re
import sys
from pathlib import Path

# "  SOME_ENV_VAR: >-" / "  X: >" -- a folded scalar header, with key indentation.
FOLDED_RE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*:\s*>[+-]?\s*(?:#.*)?$")
ENV_HEADER_RE = re.compile(r"^(\s*)env:\s*(?:#.*)?$")


def yaml_files(root):
    out = []
    wf = root / ".github" / "workflows"
    if wf.is_dir():
        out.extend(sorted(wf.glob("*.yml")) + sorted(wf.glob("*.yaml")))
    dep = root / ".github" / "dependabot.yml"
    if dep.is_file():
        out.append(dep)
    return out


def env_folded_scalars(lines):
    """Yield (lineno, line, key_indent) for lines inside folded env: scalars.

    A folded scalar belongs to env: if it sits at an indentation deeper than the
    nearest `env:` header that is still in scope (no intervening line at or above
    that header's indent other than blank/comment lines).
    """
    i = 0
    n = len(lines)
    while i < n:
        m = ENV_HEADER_RE.match(lines[i])
        if not m:
            i += 1
            continue
        env_indent = len(m.group(1))
        j = i + 1
        while j < n:
            line = lines[j]
            if line.strip() == "" or line.lstrip().startswith("#"):
                j += 1
                continue
            indent = len(line) - len(line.lstrip())
            if indent <= env_indent:
                break  # env: block ended
            fm = FOLDED_RE.match(line)
            if fm:
                header_indent = len(fm.group(1))
                k = j + 1
                while k < n:
                    cur = lines[k]
                    if cur.strip() == "":
                        yield (k + 1, cur, header_indent)
                        k += 1
                        continue
                    cindent = len(cur) - len(cur.lstrip())
                    if cindent <= header_indent:
                        break
                    yield (k + 1, cur, header_indent)
                    k += 1
                j = k
                continue
            j += 1
        i = j
    return


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args(argv)
    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parents[2]

    files = yaml_files(root)
    if not files:
        print("[FAIL] no workflow YAML files found")
        return 1

    problems = []
    inspected = 0
    have_yaml = True
    try:
        import yaml  # type: ignore
    except ImportError:
        have_yaml = False

    for p in files:
        rel = p.relative_to(root).as_posix()
        try:
            text = p.read_text(encoding="utf-8")
        except OSError as exc:
            problems.append(f"{rel}: cannot read: {exc}")
            continue

        lines = text.splitlines()

        # Rule 1a: dependency-free structural sanity (always runs).
        is_dependabot = p.name == "dependabot.yml"
        top_keys = []
        for n, line in enumerate(lines, 1):
            if "\t" in line[:len(line) - len(line.lstrip())]:
                problems.append(f"{rel}:{n}: tab indentation is invalid in YAML")
            if line and not line[0].isspace() and ":" in line and not line.lstrip().startswith("#"):
                top_keys.append(line.split(":", 1)[0].strip())
        required = ["version", "updates"] if is_dependabot else ["on", "jobs"]
        for key in required:
            if key not in top_keys:
                problems.append(f"{rel}: missing required top-level key '{key}'")
        for key in set(top_keys):
            if top_keys.count(key) > 1:
                problems.append(f"{rel}: duplicate top-level key '{key}'")

        # Rule 1a2: job skeleton, also dependency-free. A workflow with zero
        # jobs is a broken workflow. Per-step body checks are intentionally NOT
        # done here: `run:` / `uses:` can legitimately sit far below `- name:`
        # after long if:/with:/env: blocks, and GitHub's own parser is the
        # authority for step bodies. This gate owns scalar hazards, not schema.
        if not is_dependabot:
            job_lines = [l for l in lines
                         if re.match(r"^  [A-Za-z0-9_-]+:\s*$", l)]
            if not job_lines:
                problems.append(f"{rel}: no job definitions found")

        # Rule 1b: full parse when PyYAML happens to be installed.
        if have_yaml:
            try:
                yaml.safe_load(text)
            except Exception as exc:
                problems.append(f"{rel}: YAML parse error: {exc}")

        for lineno, line, _indent in env_folded_scalars(lines):
            inspected += 1
            body = line.strip()
            if not body:
                continue
            if body.startswith("#"):
                problems.append(
                    f"{rel}:{lineno}: '#' line inside a folded env: scalar is literal "
                    "content; move the comment outside the scalar")
            elif "#" in body:
                problems.append(
                    f"{rel}:{lineno}: env: scalar value contains '#'; when folded into "
                    "a shell command it comments out the remainder")

    if not have_yaml:
        print("[note] PyYAML unavailable; YAML parse check skipped, text rules still enforced")

    if inspected == 0:
        problems.append("zero folded env: scalar lines inspected across workflow files")

    if problems:
        for p in problems:
            print(f"[FAIL] {p}")
        print(f"[FAIL] {len(problems)} finding(s)")
        return 1
    print(f"[OK] {len(files)} workflow YAML file(s), "
          f"{inspected} folded env: scalar line(s) inspected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
