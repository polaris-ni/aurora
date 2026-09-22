#!/usr/bin/env python3
# ============================================================================
# check_no_hardcoded_paths.py - no machine-specific absolute paths in the repo
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §10.5 (2026-09-22 定论)
#   "仓库内禁止写死任何本机安装路径（盘符 / 用户目录一律不得入库）——换机即
#    失效且污染他人构建；需要定位外部工具/库时一律由使用者显式传入
#    （-D<选项>=<目录> 或环境变量），自动探测只作为兜底且不得含盘符。"
#
# Why a gate: 2026-09-22 found `D:/Development/Environment/LLVM/bin` hardcoded in  HARDPATH_EXEMPT: example cited in rationale, not a path present in the repo
# cmake/AuroraBackends.cmake (plus the same本机 path in CHANGELOG.json and a doc
# example). A path that is not passed in is a path that only builds on one machine.
#
# Scan scope: every version-controlled text file (git ls-files), minus
# third_party/, build*/, tests/fixtures/, .workbuddy/ and binary extensions.
#
# Blocking rules:
#   1) [blocking] User home directory literal — `C:/Users/<name>`, `/home/<name>`,
#      `/Users/<name>` (placeholder names like `user`/`you` are allowed).
#   2) [blocking] Non-system drive absolute path — `<D-Z>:/<seg>` or `<D-Z>:\<seg>`
#      (C: is the Windows system drive; other drive letters are machine-specific).
#      tests/ is exempt: 盘符本身就是若干用例的被测对象（去盘符逻辑），且夹具里的
#      "C:/work" 之类是字符串值而非构建配置。
#   3) [blocking] Machine-specific directory segment under a drive — e.g.  HARDPATH_EXEMPT: example path patterns documented for rule 3
#      `C:/Development/...`, `D:/Projects/...`, `.../msys64/...`.
#
# Exemption: a line containing "HARDPATH_EXEMPT" (case-insensitive) on the matched
# line or any of the 2 preceding lines documents a legitimate exception.
#
# Exit code: 1 when any un-exempted literal is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_no_hardcoded_paths.py [--root <aurora_root>]
# ============================================================================
import argparse
import os
import re
import subprocess
import sys

EXEMPT_TOKEN = "HARDPATH_EXEMPT"

SKIP_DIR_PREFIXES = ("third_party/", "build", "tests/fixtures/", ".workbuddy/", ".git/")
TEXT_EXTS = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh",
    ".cmake", ".txt", ".md", ".json", ".yml", ".yaml", ".toml", ".ini", ".cfg",
    ".py", ".sh", ".bash", ".ps1", ".bat", ".js", ".ts", ".rs",
)
# tests/ 豁免规则 2（盘符是被测对象/夹具字符串），但规则 1/3 仍生效。
RULE2_EXEMPT_PREFIX = "tests/"

# Placeholder account names in doc examples are not real home directories.
PLACEHOLDER_NAMES = {
    "user", "users", "username", "you", "your", "yours", "name", "me",
    "yourname", "someone", "somebody", "test", "example", "foo", "bar",
}

WIN_HOME_RE = re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+([A-Za-z0-9_.-]+)")
UX_HOME_RE = re.compile(r"(?<![\w])/(?:home|Users)/([A-Za-z0-9_.-]+)")
# Drive letters other than C: — a machine-specific volume.
OTHER_DRIVE_RE = re.compile(r"\b([D-Zd-z]):[\\/]+([A-Za-z0-9_][\w .-]*)")
# Machine-specific directory segment reached from a drive root.
NATIVE_SEG_RE = re.compile(
    r"[A-Za-z]:[\\/](?:[\w .-]+[\\/])*(Development|Projects|WorkBuddy|msys64)(?:[\\/]|$)")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def tracked_files(root):
    """Version-controlled files (git ls-files), falling back to an os.walk scan."""
    try:
        r = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True,
                           text=True, encoding="utf-8", errors="replace")
        if r.returncode == 0 and r.stdout.strip():
            return [l.strip() for l in r.stdout.splitlines() if l.strip()]
    except OSError:
        pass
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "third_party")]
        for fn in filenames:
            rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, "/")
            out.append(rel)
    return out


def is_exempt(raw_lines, idx):
    for j in range(max(0, idx - 2), idx + 1):
        if EXEMPT_TOKEN in raw_lines[j].upper():
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    violations = []
    files_scanned = 0

    for rel in tracked_files(root):
        if any(rel.startswith(p) for p in SKIP_DIR_PREFIXES):
            continue
        if not rel.endswith(TEXT_EXTS):
            continue
        ap_path = os.path.join(root, rel.replace("/", os.sep))
        if not os.path.isfile(ap_path):
            continue
        with open(ap_path, encoding="utf-8", errors="replace") as f:
            raw_lines = f.read().splitlines()
        files_scanned += 1

        for i, line in enumerate(raw_lines):
            ln = i + 1
            if is_exempt(raw_lines, i):
                continue
            hit = None

            m = WIN_HOME_RE.search(line) or UX_HOME_RE.search(line)
            if m and m.group(1).lower() not in PLACEHOLDER_NAMES:
                hit = f"user home directory literal '{m.group(0)}'"

            if not hit:
                m = NATIVE_SEG_RE.search(line)
                if m:
                    hit = (f"machine-specific path segment '{m.group(1)}' under a drive root "
                           f"('{m.group(0).strip()}')")

            if not hit and not rel.startswith(RULE2_EXEMPT_PREFIX):
                m = OTHER_DRIVE_RE.search(line)
                if m:
                    hit = (f"non-system drive absolute path '{m.group(0)}' "
                           f"(drive {m.group(1)}: is machine-specific)")

            if hit:
                violations.append(
                    f"{rel}:{ln}: {hit}\n      > {line.strip()[:160]}")

    if violations:
        print("[FAIL] hardcoded machine-specific paths found "
              "(paths must be passed in via -D<option>=<dir> or an env var, never written into the repo):")
        for v in violations:
            print(f"  {v}")
        print("  legit exceptions must carry a 'HARDPATH_EXEMPT: <reason>' comment "
              "on the line or one of the 2 lines above")
        return 1

    print(f"[PASS] no hardcoded machine-specific paths in {files_scanned} scanned files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
