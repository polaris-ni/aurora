#!/usr/bin/env python3
"""
check_i18n_gen_golden.py - golden / determinism / error-class guard for the gen_i18n pipeline.

Why this exists (the C++ leg it complements is tests/integration/itest_i18n_pipeline.cpp):
  gen_i18n turns vocabulary JSON into two products - a thin generated header and an AUM1 binary
  vocabulary - and both are committed to the repository (the fixture pair under tests/fixtures/i18n/gen/
  and the library's reserved-region header include/aurora/i18n/builtin_messages.gen.h). Committed
  generators rot silently: a validator change that loosens output bytes, a sort that disappears, a
  builtin table edited by hand. This gate re-runs the *real build artifact* and compares.

Legs:
  1. determinism        - the same command twice must produce byte-identical .hpp and .bin.
  2. argument order     - locale files shuffled on the command line must produce the same bytes
                          (the product is keyed by locale tag, not by argv order).
  3. golden parity      - regenerated + clang-format'd products must equal the committed ones
                          (the same "generate then format" pipeline cmake/AuroraTools.cmake uses for
                          error_codes.gen.h; without the format step regeneration itself reddens
                          format-check). Header legs SKIP when no usable clang-format is present;
                          the .bin and the legs above never skip.
  4. builtin same-source lock - codespec/i18n_builtin.json regenerated through --emit-builtin must
                          equal the committed header, and the fixture .bin must carry the reserved
                          region the same names map to.
  5. error classes      - each malformed input must exit non-zero, name the rule in its diagnostic,
                          and leave both output files unwritten (no half-written vocabulary).
  6. warn-only shapes   - a suspicious-but-legal template must exit 0 *with* the warning, so the
                          error/warn split cannot drift to "everything is an error".
  7. non-vacuity       - minimum counts for goldens, error cases and emitted Id constants, so an
                          accidentally emptied fixture cannot make every leg pass trivially.

Usage:
  python3 tools/check/check_i18n_gen_golden.py [build_dir] [--update] [--root PATH]
    build_dir  directory holding the built gen_i18n (default: "build")
    --update   rewrite the committed goldens instead of comparing them
    --root     repository root (default: located by walking up from this script)
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# Fixture vocabulary: the exact command line that produced tests/fixtures/i18n/gen/messages_base.*
FIXTURE_NS = "itest_i18n::msg"
FIXTURE_ID_BASE = "1024"
FIXTURE_BASE = os.path.join("tests", "fixtures", "i18n", "messages_base.json")
FIXTURE_LOCALES = [
    os.path.join("tests", "fixtures", "i18n", "messages_ru.json"),
    os.path.join("tests", "fixtures", "i18n", "messages_zh-Hans-CN.json"),
]
GOLDEN_HPP = os.path.join("tests", "fixtures", "i18n", "gen", "messages_base.hpp")
GOLDEN_BIN = os.path.join("tests", "fixtures", "i18n", "gen", "messages_base.bin")
BUILTIN_SOURCE = os.path.join("codespec", "i18n_builtin.json")
BUILTIN_HEADER = os.path.join("include", "aurora", "i18n", "builtin_messages.gen.h")

MIN_FIXTURE_ID_CONSTANTS = 8
MIN_ERROR_CASES = 10

# Malformed inputs, each pinned to the diagnostic that must name the rule. Keeping the JSON inline
# (rather than as fixture files) means a rule can be deleted only by editing this table, which is
# visible in review. `locales` entries are extra argv after the base file.
ERROR_CASES = [
    (
        "unparseable-json",
        '{"locale": "en", "messages": {',
        [],
        "JSON parse failed",
    ),
    (
        "metadata-without-entry",
        '{"locale": "en", "messages": {"a.b": "x", "@a.c": {"placeholders": {}}}}',
        [],
        "metadata '@a.c' has no matching entry",
    ),
    (
        "invalid-param-type",
        '{"locale": "en", "messages": {'
        '"a.b": "{n}", '
        '"@a.b": {"placeholders": {"n": {"type": "int32"}}}}}',
        [],
        "has an invalid type: int32",
    ),
    (
        "placeholder-without-metadata",
        '{"locale": "en", "messages": {"a.b": "{n}", "@a.b": {"placeholders": {}}}}',
        [],
        "has no @key.placeholders metadata",
    ),
    (
        "invalid-key-shape",
        '{"locale": "en", "messages": {"Bad Key": "x"}}',
        [],
        "key shape is invalid",
    ),
    (
        "selector-operand-type",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, plural, one {# file} other {# files}}", '
        '"@a.b": {"placeholders": {"n": {"type": "string"}}}}}',
        [],
        "selector operand 'n' must be int64 or double",
    ),
    (
        "select-operand-must-be-string",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, select, one {x} other {y}}", '
        '"@a.b": {"placeholders": {"n": {"type": "int64"}}}}}',
        [],
        "select operand 'n' must be string (exact match)",
    ),
    (
        "plural-missing-other",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, plural, one {# file}}", '
        '"@a.b": {"placeholders": {"n": {"type": "int64"}}}}}',
        [],
        "plural is missing the 'other' fallback branch",
    ),
    (
        "unknown-plural-category",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, plural, duotwo {#} other {#}}", '
        '"@a.b": {"placeholders": {"n": {"type": "int64"}}}}}',
        [],
        "is neither a CLDR category nor an =N exact value",
    ),
    (
        "unsupported-function",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, spellout}", '
        '"@a.b": {"placeholders": {"n": {"type": "int64"}}}}}',
        [],
        "unsupported placeholder function 'spellout'",
    ),
    (
        "date-without-style-form",
        '{"locale": "en", "messages": {'
        '"a.b": "{when}", '
        '"@a.b": {"placeholders": {"when": {"type": "date"}}}}}',
        [],
        "must be written as {name, date",
    ),
    (
        "currency-needs-numeric",
        '{"locale": "en", "messages": {'
        '"a.b": "{total, currency, USD}", '
        '"@a.b": {"placeholders": {"total": {"type": "date"}}}}}',
        [],
        "{, currency} param 'total' must be numeric",
    ),
    (
        "unknown-currency-code",
        '{"locale": "en", "messages": {'
        '"a.b": "{total, currency, ZZZ}", '
        '"@a.b": {"placeholders": {"total": {"type": "double"}}}}}',
        [],
        "currency must be a 3-letter ISO code",
    ),
    (
        "bad-date-style",
        '{"locale": "en", "messages": {'
        '"a.b": "{when, date, epic}", '
        '"@a.b": {"placeholders": {"when": {"type": "date"}}}}}',
        [],
        "date style only supports short / medium / long",
    ),
    (
        "locale-structure-drift",
        '{"locale": "en", "messages": {'
        '"a.b": "{n, plural, one {# file} other {# files}}", '
        '"@a.b": {"placeholders": {"n": {"type": "int64"}}}}}',
        [('{"locale": "fr", "messages": {"a.b": "un fichier"}}', "fr.json")],
        "has a different structure in locale 'fr'",
    ),
    (
        "locale-invents-select-value",
        '{"locale": "en", "messages": {'
        '"a.b": "{who, select, admin {A} other {U}}", '
        '"@a.b": {"placeholders": {"who": {"type": "string"}}}}}',
        [('{"locale": "fr", "messages": {"a.b": '
          '"{who, select, admin {A} guest {G} other {U}}"}}', "fr.json")],
        "adds select value 'who=guest'",
    ),
    (
        "invalid-locale-tag",
        '{"locale": "en", "messages": {"a.b": "x"}}',
        [('{"locale": "english", "messages": {"a.b": "x"}}', "bad.json")],
        "locale tag shape is invalid",
    ),
    (
        "base-redefines-builtin-key",
        '{"locale": "en", "messages": {"aurora.reorder.position": "Mine"}}',
        [],
        "is a library builtin message name",
    ),
    (
        "id-constant-collision",
        '{"locale": "en", "messages": {"a.b": "x", "a_b": "y"}}',
        [],
        "Id constant name collides after flattening the dots",
    ),
]

# id_base inside the reserved region is a CLI-level rejection, so it gets its own case shape.
ID_BASE_CASE = (
    '{"locale": "en", "messages": {"a.b": "x"}}',
    "512",
    "must not fall inside the builtin reserved region",
)

# Legal but suspicious: the gate must see exit 0 *plus* the warning (the error/warn split stays honest).
WARN_CASES = [
    (
        "hash-without-numeric-context",
        '{"locale": "en", "messages": {"a.b": "{who, select, admin {#} other {#}}", '
        '"@a.b": {"placeholders": {"who": {"type": "string"}}}}}',
        "renders as a literal '#'",
    ),
    (
        "unused-metadata-param",
        '{"locale": "en", "messages": {"a.b": "plain text", '
        '"@a.b": {"placeholders": {"spare": {"type": "string"}}}}}',
        "is never used by the template",
    ),
]


def repo_root_of(path):
    """Walk up to the repository root (the directory holding CMakeLists.txt)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def resolve_exe(build_dir, name):
    """Locate a built tool, accounting for multi-config generators (VS / Xcode put outputs in
    build_dir/<CONFIG>/). Mirrors tools/check/check_gen_api_merge.py."""
    suffix = ".exe" if os.name == "nt" else ""
    base = name + suffix
    candidates = [os.path.join(build_dir, base)]
    if os.name == "nt":
        for cfg in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
            candidates.append(os.path.join(build_dir, cfg, base))
    for path in candidates:
        if os.path.isfile(path):
            return path
    return candidates[0]


def clang_format_exe(repo, pinned=None):
    """Find a clang-format that can parse this repo's config; None when unavailable.

    `--clang-format` (pinned) wins: cmake/AuroraTests.cmake hands the gate the very executable the
    format gate uses, so a build whose PATH lacks clang-format still gets the header legs covered.
    The pinned build matters otherwise: the config uses enum values distro packages reject
    (`BinPackParameters: BinPack`), and a build that fails to dump the config cannot format
    anything (see cmake/AuroraUtils.cmake)."""
    candidates = []
    if pinned:
        candidates.append(pinned)
    env = os.environ.get("AURORA_CLANG_FORMAT_EXE")
    if env:
        candidates.append(env)
    found = shutil.which("clang-format")
    if found:
        candidates.append(found)
    style = "file:" + os.path.join(repo, ".clang-format")
    for candidate in candidates:
        probe = subprocess.run([candidate, "--dump-config", "--style", style],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            return candidate
        if pinned:
            # The caller pinned a build that cannot read this config: trying PATH behind its back
            # would silently format with a different clang-format than the format gate uses.
            return None
    return None


def format_file(exe, repo, path):
    """Rewrite *path* in the repo style (the style path is pinned so a build directory outside the
    repository cannot silently fall back to the LLVM built-in style)."""
    subprocess.run([exe, "-i", "--style", "file:" + os.path.join(repo, ".clang-format"), path],
                   check=True)


def rel(path):
    """Repository-relative path with forward slashes, for messages (os.path.join uses the host separator).
    """
    return path.replace(os.sep, "/")


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def run_generator(exe, cwd, out_dir, out_name, namespace, id_base, base_file, locale_files):
    """Invoke gen_i18n for one product pair; returns (returncode, stderr, hpp_bytes, bin_bytes)."""
    out_hpp = os.path.join(out_dir, out_name + ".hpp")
    out_bin = os.path.join(out_dir, out_name + ".bin")
    argv = [exe, out_hpp, out_bin, namespace, "--id-base", id_base, base_file, *locale_files]
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    hpp = read_bytes(out_hpp) if os.path.isfile(out_hpp) else None
    bin_bytes = read_bytes(out_bin) if os.path.isfile(out_bin) else None
    return result.returncode, result.stderr, hpp, bin_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("build_dir", nargs="?", default="build")
    parser.add_argument("--root", default=None)
    parser.add_argument("--clang-format", default=None,
                        help="pin the clang-format build the format gate uses (cmake passes it)")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the committed goldens instead of comparing")
    args = parser.parse_args()

    repo = args.root or repo_root_of(__file__)
    build = args.build_dir if os.path.isabs(args.build_dir) else os.path.join(repo, args.build_dir)
    exe = resolve_exe(build, "gen_i18n")
    if not os.path.isfile(exe):
        print("[FAIL] gen_i18n not built: %s\n       run `cmake --build %s --target gen_i18n` first"
              % (exe, build))
        return 2

    cf = clang_format_exe(repo, args.clang_format)
    failures = []
    checks = []
    header_legs = 0

    def note(text):
        checks.append(text)

    def fail(text):
        failures.append(text)

    with tempfile.TemporaryDirectory() as tmp:
        # ---- Legs 1 and 2: determinism, then CLI-order independence ----
        rc1, err1, hpp1, bin1 = run_generator(exe, repo, tmp, "run1", FIXTURE_NS, FIXTURE_ID_BASE,
                                              FIXTURE_BASE, FIXTURE_LOCALES)
        rc2, _, hpp2, bin2 = run_generator(exe, repo, tmp, "run2", FIXTURE_NS, FIXTURE_ID_BASE,
                                           FIXTURE_BASE, FIXTURE_LOCALES)
        rc3, _, hpp3, bin3 = run_generator(exe, repo, tmp, "run3", FIXTURE_NS, FIXTURE_ID_BASE,
                                           FIXTURE_BASE, list(reversed(FIXTURE_LOCALES)))
        if rc1 != 0 or hpp1 is None or bin1 is None:
            print("[FAIL] fixture generation failed (exit=%s):\n%s" % (rc1, err1))
            return 1
        if (rc2, hpp2, bin2) != (0, hpp1, bin1):
            fail("repeat run produced different bytes (output is not deterministic)")
        if (rc3, hpp3, bin3) != (0, hpp1, bin1):
            fail("shuffling the locale argv order changed the product bytes")
        else:
            note("determinism: two runs and a shuffled argv order are byte-identical")

        if hpp1.count(b"inline constexpr aurora::MessageId") < MIN_FIXTURE_ID_CONSTANTS:
            fail("fixture generated fewer than %d Id constants (fixture emptied?)"
                 % MIN_FIXTURE_ID_CONSTANTS)
        if not bin1.startswith(b"AUM1"):
            fail("fixture bin lost the AUM1 magic")

        # ---- Leg 3: golden parity (header compared after the repo's format step) ----
        staged_hpp = os.path.join(tmp, "golden.hpp")
        with open(staged_hpp, "wb") as handle:
            handle.write(hpp1)
        if cf:
            format_file(cf, repo, staged_hpp)
            header_legs += 1
            committed = read_bytes(os.path.join(repo, GOLDEN_HPP))
            produced = read_bytes(staged_hpp)
            if args.update:
                shutil.copyfile(staged_hpp, os.path.join(repo, GOLDEN_HPP))
                note("updated %s" % rel(GOLDEN_HPP))
            elif produced != committed:
                fail("golden drift: regenerated header != %s (re-run gen_i18n + clang-format, "
                 "or use --update)" % rel(GOLDEN_HPP))
            else:
                note("golden parity: fixture header matches the committed product")
        else:
            note("SKIP header parity (no usable clang-format found)")

        committed_bin = read_bytes(os.path.join(repo, GOLDEN_BIN))
        if args.update:
            shutil.copyfile(os.path.join(tmp, "run1.bin"), os.path.join(repo, GOLDEN_BIN))
            note("updated %s" % rel(GOLDEN_BIN))
        elif committed_bin != bin1:
            fail("golden drift: regenerated AUM1 bin != %s" % rel(GOLDEN_BIN))
        else:
            note("golden parity: fixture bin matches the committed product")

        # ---- Leg 4: builtin reserved-region same-source lock ----
        builtin_hpp = os.path.join(tmp, "builtin.hpp")
        result = subprocess.run([exe, "--emit-builtin", builtin_hpp, BUILTIN_SOURCE],
                                cwd=repo, capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        if result.returncode != 0 or not os.path.isfile(builtin_hpp):
            fail("--emit-builtin failed (exit=%s):\n%s" % (result.returncode, result.stderr))
        else:
            if cf:
                format_file(cf, repo, builtin_hpp)
                header_legs += 1
                committed_builtin = read_bytes(os.path.join(repo, BUILTIN_HEADER))
                if args.update:
                    shutil.copyfile(builtin_hpp, os.path.join(repo, BUILTIN_HEADER))
                    note("updated %s" % rel(BUILTIN_HEADER))
                elif read_bytes(builtin_hpp) != committed_builtin:
                    fail("builtin lock: %s is not the product of %s (hand-edited, or the source "
                         "changed without regenerating)" % (rel(BUILTIN_HEADER), rel(BUILTIN_SOURCE)))
                else:
                    note("builtin lock: reserved-region header matches its single source")
            else:
                note("SKIP builtin header parity (no usable clang-format found)")
            # The merged fixture bin must carry the reserved region the header declares: the ru
            # override of aurora.reorder.position is only reachable when both sides index id 4.
            if b"aurora.reorder.position" not in bin1:
                fail("builtin lock: fixture bin does not index the reserved region")

        # ---- Leg 5: error classes (exit non-zero, name the rule, write nothing) ----
        err_dir = os.path.join(tmp, "errors")
        os.makedirs(err_dir)
        for name, base_json, locale_specs, needle in ERROR_CASES:
            case_dir = os.path.join(err_dir, name)
            os.makedirs(case_dir)
            base_path = os.path.join(case_dir, "base.json")
            write_text(base_path, base_json)
            locale_paths = []
            for text, filename in locale_specs:
                locale_path = os.path.join(case_dir, filename)
                write_text(locale_path, text)
                locale_paths.append(locale_path)
            code, stderr, hpp, bin_bytes = run_generator(exe, repo, case_dir, "out", "app::msg",
                                                         "1024", base_path, locale_paths)
            if code == 0:
                fail("error class '%s' was accepted (exit 0)" % name)
            elif needle not in stderr:
                fail("error class '%s' rejected without naming the rule; expected %r in:\n%s"
                     % (name, needle, stderr))
            elif hpp is not None or bin_bytes is not None:
                fail("error class '%s' still wrote an output file" % name)
        if len(ERROR_CASES) < MIN_ERROR_CASES:
            fail("fewer than %d error cases registered (guard weakened?)" % MIN_ERROR_CASES)
        # id_base inside the reserved region is rejected before any product is written.
        base_only = os.path.join(err_dir, "id_base.json")
        write_text(base_only, ID_BASE_CASE[0])
        code, stderr, hpp, bin_bytes = run_generator(exe, repo, err_dir, "id_base_out", "app::msg",
                                                     ID_BASE_CASE[1], base_only, [])
        if code == 0:
            fail("--id-base inside the reserved region was accepted")
        elif ID_BASE_CASE[2] not in stderr:
            fail("--id-base rejection did not name the reserved region:\n%s" % stderr)
        elif hpp is not None or bin_bytes is not None:
            fail("--id-base rejection still wrote an output file")
        note("error classes: %d malformed inputs rejected, %d wrote nothing"
             % (len(ERROR_CASES) + 1, len(ERROR_CASES) + 1))

        # ---- Leg 6: warn-only shapes must still produce ----
        warn_dir = os.path.join(tmp, "warns")
        os.makedirs(warn_dir)
        for name, base_json, needle in WARN_CASES:
            case_dir = os.path.join(warn_dir, name)
            os.makedirs(case_dir)
            base_path = os.path.join(case_dir, "base.json")
            write_text(base_path, base_json)
            code, stderr, hpp, bin_bytes = run_generator(exe, repo, case_dir, "out", "app::msg",
                                                         "1024", base_path, [])
            if code != 0 or hpp is None:
                fail("warn-only shape '%s' was rejected (exit %s):\n%s" % (name, code, stderr))
            elif needle not in stderr:
                fail("warn-only shape '%s' produced no warning containing %r" % (name, needle))
        note("warn-only shapes: %d legal-but-suspicious templates produced with a warning"
             % len(WARN_CASES))

    if failures:
        print("[FAIL] i18n golden pipeline check failed:")
        for item in failures:
            print("  - " + item)
        return 1
    print("[PASS] i18n golden pipeline check passed (%d legs, %d header legs formatted by %s)"
          % (len(checks), header_legs, cf or "none"))
    for item in checks:
        print("  * " + item)
    return 0


if __name__ == "__main__":
    sys.exit(main())
