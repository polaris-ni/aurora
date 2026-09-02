#!/usr/bin/env python3
# ============================================================================
# gen_api merge-no-truncation regression check
# ----------------------------------------------------------------------------
# Directly invokes the real build artifacts (gen_error_codes / gen_debug_api) to verify:
#   1) when the existing aurora_api.json is corrupt, the generator exits non-zero and
#      **does not write the file** (no truncation);
#   2) in the merge-only scenario, the generator writes only its own sections, preserving
#      other sections such as widgets/enums/error_codes.
#
# Usage:
#   python tools/check/check_gen_api_merge.py <build_dir> [repo_root]
#   (build_dir defaults to "build"; repo_root defaults to the repo root where the script lives)
# ============================================================================
import json
import os
import shutil
import subprocess
import sys
import tempfile


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt (still resolves correctly when the
    script lives under tools/check/)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def run(exe, *args, cwd):
    return subprocess.run([exe, *args], cwd=cwd, capture_output=True, text=True)


def main() -> int:
    repo = sys.argv[2] if len(sys.argv) > 2 else repo_root_of(__file__)
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    build_abs = build if os.path.isabs(build) else os.path.join(repo, build)

    # Cross-platform executable name: build artifacts carry a .exe suffix on Windows, but not on
    # other platforms (Linux/macOS/gcov).
    _suffix = ".exe" if os.name == "nt" else ""
    gen_err = os.path.join(build_abs, "gen_error_codes" + _suffix)
    gen_dbg = os.path.join(build_abs, "gen_debug_api" + _suffix)
    errors_toml = os.path.join(repo, "codespec", "errors.toml")
    debug_toml = os.path.join(repo, "codespec", "debug_api.toml")

    missing = [p for p in (gen_err, gen_dbg) if not os.path.exists(p)]
    if missing:
        print(
            f"[FAIL] generators not built: {missing}\n       run `cmake --build {build} --target gen_error_codes gen_debug_api` first")
        return 2

    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        # ---- Test 1: gen_error_codes should refuse to write when the existing file is corrupt (no truncation) ----
        api1 = os.path.join(tmp, "a1.json")
        # Truly unparseable content (incomplete structure; nlohmann operator>> throws parse_error)
        with open(api1, "w", encoding="utf-8") as f:
            f.write('{"widgets":[1,2],')  # unterminated, parse fails
        before = open(api1, "rb").read()
        r1 = run(gen_err, errors_toml, os.path.join(tmp, "x.gen.h"),
                 os.path.join(tmp, "x.md"), api1, cwd=tmp)
        after = open(api1, "rb").read()
        if r1.returncode == 0:
            failures.append("gen_error_codes did not reject the corrupt file (exit=0)")
        if after != before:
            failures.append("gen_error_codes still modified the file while rejecting (truncation risk)")

        # ---- Test 2: gen_debug_api should refuse to write when the existing file is corrupt (no truncation) ----
        api2 = os.path.join(tmp, "a2.json")
        with open(api2, "w", encoding="utf-8") as f:
            f.write('{"widgets":[1,2],')  # unterminated, parse fails
        before = open(api2, "rb").read()
        r2 = run(gen_dbg, debug_toml, api2, cwd=tmp)
        after = open(api2, "rb").read()
        if r2.returncode == 0:
            failures.append("gen_debug_api did not reject the corrupt file (exit=0)")
        if after != before:
            failures.append("gen_debug_api still modified the file while rejecting (truncation risk)")

        # ---- Test 3: merge-only preserves the other sections ----
        api3 = os.path.join(tmp, "a3.json")
        with open(api3, "w", encoding="utf-8") as f:
            json.dump({"widgets": [1, 2], "enums": [3], "debug": []}, f)
        r3 = run(gen_dbg, debug_toml, api3, cwd=tmp)
        if r3.returncode != 0:
            failures.append(f"gen_debug_api merge failed: {r3.stderr.strip()}")
        else:
            doc = json.load(open(api3, encoding="utf-8"))
            if doc.get("widgets") != [1, 2] or doc.get("enums") != [3]:
                failures.append("gen_debug_api merge dropped the widgets/enums sections")
            if not isinstance(doc.get("debug"), list) or len(doc["debug"]) == 0:
                failures.append("gen_debug_api merge did not write the debug section")

    if failures:
        print("[FAIL] gen_api merge regression check failed:")
        for f in failures:
            print("  - " + f)
        return 1
    print("[PASS] gen_api merge-no-truncation regression check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
