#!/usr/bin/env python3
# ============================================================================
# code <-> doc sync guard
# ----------------------------------------------------------------------------
# 低误报、确定性可判定的代码-文档一致性门禁，接入 CTest（check_code_doc_sync）。
#
# 规则（测试头部，编号 TEST-R1–TEST-R10，定义见 codespec/CODING_STANDARDS.md §3.2）：
#   DOC1    代码注释中的 `架构 §N` 引用 → codespec/ARCHITECTURE.md 必须存在该章节；
#   DOC2    代码注释中的 `规格 §N` 引用 → 需求 #N 必须存在于 SPECIFICATIONS.md 特性表（#1–#24）；
#   TEST-R1 测试头部必须含标准三行块：/// 测试类型 / /// 目标单元 / /// 测试说明
#           （历史 // 目标源单元： 约定不计入标准块，视为 TEST-R1 违规，须归一）；
#   TEST-R2 测试头部「目标单元 / 目标源单元」声明路径必须真实存在（防注释路径烂掉）；
#   TEST-R4 测试目标单元不得指向聚合入口头（aurora.h / aurora_fwd.h / aurora_pch.h）；
#   TEST-R5 每个公共单元头应被某测试显式声明为目标单元，或在该测试中直接 #include
#           （header-only 工具类型豁免），否则记为覆盖缺口；
#   TEST-R7 并行安全：测试体系禁止新增 RUN_SERIAL（CMake 编排与测试源一并扫描），
#           申请串行须列入 TEST_R7_WHITELIST 并注明根因（并行模型 = 进程隔离 + 资源虚拟化）；
#   TEST-R8 命名纪律：目录定类型 + 前缀强制——tests/unit/ 一律 utest_、tests/integration/
#           一律 itest_；测试 TU 不得放在两目录之外；禁止自定义套件宏（AURORA_TEST_NAMED 等，
#           Suite 恒等于文件 stem，见 §3.1）；
#   TEST-R9 测试 TU 禁止 using-directive（与 clang-tidy google-build-using-namespace 同口径）。
#   （TEST-R3 一一对应 / TEST-R10 跨域 catch-all 趋势不纳入硬门禁：靠评审与审计，见 §3.2；
#     TEST-R6 注册完整性由 tools/check/check_test_registry.py 的 registry_integrity 用例守门。）
#
# 设计原则：只做确定性判定。TEST-R1/TEST-R2/TEST-R4/TEST-R5 当前为存量红线——基线即现状问题数，
# 整改归零后转绿；新增违规立即转红（防回退）。白名单仅用于文档引用类存量豁免。
#
# Usage:
#   python tools/check/check_code_doc_sync.py [repo_root]
# ============================================================================
import os
import re
import sys

# ---- 白名单：文档引用类存量豁免，逐项注明原因 ------------------------------
# 键为 (规则, 仓库相对路径, 详情子串)。当前为空：原 3 条 TEST-R2 豁免指向
# utest_aurora_lsp / utest_default_construct / utest_todo，这些文件已在「破旧」阶段
# 删除，豁免成为无匹配目标的孤儿，随守门重写一并清理。
WHITELIST = {}

# ---- TEST-R7 串行白名单：申请串行的测试条目，逐项注明根因 -------------------
# 键为仓库相对路径（相对 CMake 编排文件则写 "cmake/<文件>#<测试名>"）。
# 并行模型 = CTest 进程隔离 + 框架用例边界资源虚拟化（见 CODING_STANDARDS.md §3.1），
# 新增 RUN_SERIAL 属于违规；确因外部资源无法虚拟化而需要串行的，先在此登记根因。
TEST_R7_WHITELIST = {}

# TEST-R10 趋势基线：跨 ≥3 模块域的测试文件（catch-all 反模式）允许存量上限。
CATCH_ALL_BASELINE = 20

# TEST-R5 全局硬门禁（收束阶段已恢复）：每个公共单元头必须被某测试覆盖
# （声明为目标单元 / 直接 #include / 符号被 tests/ 引用，header-only 工具类型豁免）。
TEST_R5_ENFORCED = True

ARCH_REF_RE = re.compile(r"架构\s*§\s*([\d.]+)")
SPEC_REF_RE = re.compile(r"规格\s*§\s*([\d.]+)")
# 只校验「目标单元 / 目标源单元」（路径声明）；「目标组合」是语义描述，不是路径。
TARGET_RE = re.compile(r"目标(?:单元|源单元)[:：]\s*([^\s`]+)")
PLACEHOLDER_RE = re.compile(r"[（(<*]待补|TODO|TBD|xxx", re.IGNORECASE)
HEADING_NUM_RE = re.compile(r"^(#{1,6})\s+(\d+(?:\.\d+)*)[\s、.]")

# ---- 测试头部解析（与 TEST-R1–TEST-R6 同口径）------------------------------
TYPE_RE = re.compile(r"///\s*测试类型\s*[:：]\s*(.+)")
STD_TARGET_RE = re.compile(r"///\s*目标单元\s*[:：]\s*(.+)")
ALT_TARGET_RE = re.compile(r"//\s*目标源单元\s*[:：]\s*(.+)")
NOTE_RE = re.compile(r"///\s*测试说明\s*[:：]\s*(.+)")
INCLUDE_RE = re.compile(r'#\s*include\s+"(aurora/([A-Za-z0-9_]+)/([A-Za-z0-9_.\-]+)\.h)"')
MAP_TOKEN_RE = re.compile(r"(?:include/aurora/)?([a-z_]+)/([a-z0-9_.\-]+)\.(h|cpp)")
MODULE_DOMAINS = {
    "animation", "app", "core", "debug", "environment", "event", "i18n", "image",
    "inspector", "layout", "media", "modifier", "navigation", "perf", "preferences",
    "render", "state", "storage", "theming", "ui", "widget", "window",
}
AGG_HEADERS = {"include/aurora/aurora.h", "include/aurora/aurora_fwd.h",
               "include/aurora/aurora_pch.h"}
HEADER_ONLY_EXEMPT = {
    "include/aurora/core/assert.h", "include/aurora/core/color.h",
    "include/aurora/core/math.h", "include/aurora/core/types.h",
    "include/aurora/core/literals.h", "include/aurora/core/utf8.h",
}

CODE_DIRS = ("include", "src", "tools")
SOURCE_EXT = (".h", ".hpp", ".cpp")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def iter_source_files(repo):
    for directory in CODE_DIRS:
        base = os.path.join(repo, directory)
        if not os.path.isdir(base):
            continue
        for current, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ("build", ".git")]
            for name in sorted(files):
                if name.endswith(SOURCE_EXT):
                    yield os.path.relpath(os.path.join(current, name), repo)


def section_numbers(md_path):
    if not os.path.isfile(md_path):
        return None
    numbers = set()
    with open(md_path, encoding="utf-8") as handle:
        for line in handle:
            match = HEADING_NUM_RE.match(line)
            if match:
                numbers.add(match.group(2))
    return numbers


def feature_numbers(spec_path):
    if not os.path.isfile(spec_path):
        return None
    numbers = set()
    with open(spec_path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^\s*\|\s*(\d{1,2})\s*\|", line)
            if match:
                value = int(match.group(1))
                if 1 <= value <= 24:
                    numbers.add(value)
    return numbers


def check_doc_refs(repo, problems):
    """DOC1/DOC2: `架构 §N` and `规格 §N` references in code comments must resolve."""
    arch_numbers = section_numbers(os.path.join(repo, "codespec", "ARCHITECTURE.md"))
    spec_numbers = feature_numbers(os.path.join(repo, "codespec", "SPECIFICATIONS.md"))
    for rel in iter_source_files(repo):
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
            for lineno, line in enumerate(handle, start=1):
                if not line.lstrip().startswith(("//", "///", "*", "/*")):
                    continue
                for number in ARCH_REF_RE.findall(line):
                    if arch_numbers is not None and number not in arch_numbers:
                        problems.append(("DOC1", rel, lineno, f"架构 §{number} not found in ARCHITECTURE.md"))
                for raw in SPEC_REF_RE.findall(line):
                    try:
                        number = int(float(raw))
                    except ValueError:
                        continue
                    if spec_numbers is not None and number not in spec_numbers:
                        problems.append(("DOC2", rel, lineno, f"规格 §{number} is not a known requirement #N"))


def resolve_target(repo, target):
    """Resolve a declared target to a repo-root-relative path, or None if it does not exist."""
    candidates = (target,
                  os.path.join("include", "aurora", target),
                  os.path.join("src", "aurora", target),
                  os.path.join("include", target),
                  os.path.join("src", target))
    for c in candidates:
        rp = c.replace("\\", "/")
        if os.path.exists(os.path.join(repo, rp)):
            return rp
    return None


def target_exists(repo, target):
    """Whether a declared target path resolves to a real file."""
    return resolve_target(repo, target) is not None


def split_targets(val):
    if not val:
        return []
    parts = re.split(r"[+\s、,；;]+", val)
    out = []
    for p in parts:
        p = p.strip().strip("`").strip('"').strip("'").rstrip(")").strip()
        if re.search(r"/.*\.(h|cpp)$", p):
            out.append(p)
    return out


def parse_test_header(repo, rel):
    """Parse the header block of a test file (first 40 lines).

    Returns: format ('standard'|'alt'|'none'), declared resolved headers,
    primary resolved target, whether primary is an aggregate header,
    and directly #include'd module headers.
    """
    full = os.path.join(repo, rel)
    with open(full, encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()[:40]
    type_v = note_v = std_v = alt_v = None
    for line in lines:
        s = line.lstrip()
        if not s.startswith("///") and not s.startswith("//"):
            continue
        m = TYPE_RE.match(line)
        if m and type_v is None:
            type_v = m.group(1).strip(); continue
        m = NOTE_RE.match(line)
        if m and note_v is None:
            note_v = m.group(1).strip(); continue
        m = STD_TARGET_RE.match(line)
        if m and std_v is None:
            std_v = m.group(1).strip(); continue
        m = ALT_TARGET_RE.match(line)
        if m and alt_v is None:
            alt_v = m.group(1).strip(); continue
    has_std_block = (type_v is not None and std_v is not None and note_v is not None)
    fmt = "standard" if has_std_block else ("alt" if alt_v is not None else "none")

    declared_tokens = []
    if std_v:
        declared_tokens += split_targets(std_v)
    if alt_v:
        declared_tokens += split_targets(alt_v)
    for line in lines:
        if not line.lstrip().startswith("//"):
            continue
        for m in MAP_TOKEN_RE.finditer(line):
            dom, fname, ext = m.group(1), m.group(2), m.group(3)
            if dom not in MODULE_DOMAINS or dom == "detail":
                continue
            if fname in ("aurora", "aurora_fwd", "aurora_pch"):
                continue
            if fname.endswith("_test") or fname.startswith("test_"):
                continue
            declared_tokens.append(f"{dom}/{fname}.{ext}")

    declared_resolved = set()
    for tk in declared_tokens:
        r = resolve_target(repo, tk)
        if r:
            declared_resolved.add(r.replace("\\", "/"))

    target_raw = std_v or alt_v
    is_placeholder = bool(target_raw) and bool(PLACEHOLDER_RE.search(target_raw))
    primary = resolve_target(repo, std_v) if (std_v and not is_placeholder) else None
    target_is_agg = (primary in AGG_HEADERS) if primary \
        else (target_raw in ("include/aurora/aurora.h", "aurora.h") if target_raw else False)

    incl_headers = set()
    with open(full, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            m = INCLUDE_RE.match(line.strip())
            if m and m.group(2) in MODULE_DOMAINS and m.group(2) != "detail":
                incl_headers.add(("include/" + m.group(1)).replace("\\", "/"))

    return {"format": fmt, "declared": declared_resolved, "primary": primary,
            "target_is_agg": target_is_agg, "target_raw": target_raw,
            "is_placeholder": is_placeholder, "incl_headers": incl_headers}


def collect_public_unit_headers(repo):
    headers = []
    base = os.path.join(repo, "include", "aurora")
    for cur, dirs, files in os.walk(base):
        for name in files:
            if not name.endswith(".h"):
                continue
            rp = os.path.relpath(os.path.join(cur, name), repo).replace("\\", "/")
            if "/detail/" in rp or name.endswith(".gen.h") or rp in AGG_HEADERS \
               or rp in HEADER_ONLY_EXEMPT:
                continue
            headers.append(rp)
    return sorted(headers)


def check_test_headers(repo, problems):
    """TEST-R1 (header completeness) / TEST-R2 (target path valid) / TEST-R4 (no aggregate target).

    Scoped to tests/unit (the remediation target); integration tests follow their own
    conventions and are out of scope for these rules.
    """
    base = os.path.join(repo, "tests", "unit")
    if not os.path.isdir(base):
        return
    for name in sorted(os.listdir(base)):
        if not (name.startswith("utest_") and name.endswith(".cpp")):
            continue
        rel = os.path.join("tests/unit", name).replace("\\", "/")
        info = parse_test_header(repo, rel)
        if info["format"] != "standard":
            detail = "头部缺标准三行块 (/// 测试类型/目标单元/测试说明)" + \
                     ("；使用历史 // 目标源单元： 约定" if info["format"] == "alt" else "；无任何目标声明")
            problems.append(("TEST-R1", rel, 1, detail))
        if info["is_placeholder"]:
            problems.append(("TEST-R2", rel, 1, info["target_raw"]))
            continue
        if info["target_raw"] and info["primary"] is None and not info["declared"]:
            problems.append(("TEST-R2", rel, 1, "目标单元路径不存在: " + info["target_raw"]))
        if info["target_is_agg"]:
            problems.append(("TEST-R4", rel, 1, "目标单元指向聚合头: " + (info["target_raw"] or "")))


def symbol_referenced(repo, stem):
    """Whether a header's stem (or PascalCase form) appears anywhere in tests/."""
    pas = "".join(w.capitalize() for w in stem.split("_"))
    pat = re.compile(r"\b(" + re.escape(pas) + r"|" + re.escape(stem) + r")\b")
    tests_root = os.path.join(repo, "tests")
    for cur, dirs, files in os.walk(tests_root):
        dirs[:] = [d for d in dirs if d not in ("build", ".git")]
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            fp = os.path.join(cur, fn)
            try:
                with open(fp, encoding="utf-8", errors="replace") as fh:
                    if pat.search(fh.read()):
                        return True
            except OSError:
                pass
    return False


def check_public_header_coverage(repo, problems):
    """TEST-R5: every public unit header must be covered by a test.

    Covered = declared as a target OR directly #include'd in some unit test OR its
    symbol referenced anywhere under tests/. The last clause separates genuinely
    orphaned headers (no test exercises them) from those pulled in via the aurora.h
    umbrella — only the former are hard gaps.
    """
    unit_headers = set(collect_public_unit_headers(repo))
    declared_all = set()
    effective = set()
    base = os.path.join(repo, "tests", "unit")
    if os.path.isdir(base):
        for name in sorted(os.listdir(base)):
            if not (name.startswith("utest_") and name.endswith(".cpp")):
                continue
            rel = os.path.join("tests/unit", name).replace("\\", "/")
            info = parse_test_header(repo, rel)
            declared_all |= (info["declared"] & unit_headers)
            effective |= (info["incl_headers"] & unit_headers)
    for h in sorted(unit_headers - declared_all - effective):
        stem = os.path.splitext(os.path.basename(h))[0]
        if not symbol_referenced(repo, stem):
            problems.append(("R5", h, 1, "公共单元头无测试覆盖（未声明为目标、未被直接 include、符号零引用）"))


def _strip_line_and_block_comments(text):
    """Comment stripper for TEST-R7/R8/R9 source scans (commented-out code must not count)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _iter_tests_tree(repo, extensions=(".cpp", ".h")):
    """Yield repo-relative paths of all sources under tests/ (framework included)."""
    root = os.path.join(repo, "tests")
    for current, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in ("build", ".git")]
        for name in sorted(files):
            if name.endswith(extensions):
                yield os.path.relpath(os.path.join(current, name), repo).replace("\\", "/")


def check_parallel_safety(repo, problems):
    """TEST-R7: no RUN_SERIAL anywhere in the test system.

    Scans both the CMake orchestration (where the old runner injected it) and test
    sources; hits not whitelisted in TEST_R7_WHITELIST are violations. The parallel
    model is CTest process isolation + framework case-boundary resource virtualization
    (isolation.h), so serial execution is never the answer for flakiness.
    """
    cmake_files = ["cmake/AuroraTests.cmake", "tests/CMakeLists.txt"]
    for rel in cmake_files:
        if rel in TEST_R7_WHITELIST:
            continue
        path = os.path.join(repo, rel)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = re.sub(r"#[^\n]*", " ", handle.read())  # CMake 注释以 # 开头
        for lineno, line in enumerate(text.splitlines(), start=1):
            if "RUN_SERIAL" in line:
                problems.append(("TEST-R7", rel, lineno, "测试体系禁止 RUN_SERIAL（并行=进程隔离+资源虚拟化）；"
                                                         "确需串行先登记 TEST_R7_WHITELIST 并注明根因"))
    for rel in _iter_tests_tree(repo, (".cpp",)):
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
            text = _strip_line_and_block_comments(handle.read())
        for lineno, line in enumerate(text.splitlines(), start=1):
            if "RUN_SERIAL" in line and rel not in TEST_R7_WHITELIST:
                problems.append(("TEST-R7", rel, lineno, "测试源中出现 RUN_SERIAL（应由资源虚拟化消除，而非串行）"))


def check_test_naming(repo, problems):
    """TEST-R8: directory defines type + mandatory prefix; no stray test TU; no custom suite macro.

    Suite == file stem is enforced by the framework (derived from __FILE__), so the
    static check targets the ways that contract used to be (or could be) broken:
    wrongly named/prefaced files, test TUs outside tests/unit|integration, and
    custom-suite macros that would bypass the stem binding.
    """
    forbidden_macros = re.compile(r"\bAURORA_TEST_NAMED\b|\bAURORA_TEST_SUITE\b")
    for kind, prefix in (("unit", "utest_"), ("integration", "itest_")):
        base = os.path.join(repo, "tests", kind)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not name.endswith(".cpp"):
                continue
            rel = os.path.join("tests", kind, name).replace("\\", "/")
            if not name.startswith(prefix):
                problems.append(("TEST-R8", rel, 1, f"tests/{kind}/ 测试文件须以 {prefix} 前缀命名"))
            with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
                code = _strip_line_and_block_comments(handle.read())
            for lineno, line in enumerate(code.splitlines(), start=1):
                if forbidden_macros.search(line):
                    problems.append(("TEST-R8", rel, lineno, "自定义套件宏被禁止：Suite 恒等于文件 stem（__FILE__ 推导）"))
    # 测试 TU 只能在 tests/unit 与 tests/integration（框架/支撑目录豁免）。
    for rel in _iter_tests_tree(repo, (".cpp",)):
        parts = rel.split("/")
        if len(parts) == 2 and parts[0] == "tests":  # tests/<file>.cpp —— 游离在类型目录之外
            problems.append(("TEST-R8", rel, 1, "测试 TU 须位于 tests/unit/ 或 tests/integration/（目录定类型）"))


def check_using_directive(repo, problems):
    """TEST-R9: no using-directive in test TUs (clang-tidy google-build-using-namespace 同口径).

    `using namespace` inside functions is still a using-directive and is equally
    forbidden; using declarations and namespace aliases are the sanctioned forms.
    """
    for rel in _iter_tests_tree(repo, (".cpp", ".h")):
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
            code = _strip_line_and_block_comments(handle.read())
        for lineno, line in enumerate(code.splitlines(), start=1):
            if re.search(r"\busing\s+namespace\b", line):
                problems.append(("TEST-R9", rel, lineno, "测试代码禁止 using-directive；用 using 声明 / 命名空间别名 / 显式限定"))


def report_catch_all_trend(repo):
    """TEST-R10 (trend, report-only): test files spanning >=3 module domains."""
    counts = {}
    for kind in ("unit", "integration"):
        base = os.path.join(repo, "tests", kind)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not name.endswith(".cpp"):
                continue
            rel = os.path.join("tests", kind, name).replace("\\", "/")
            info = parse_test_header(repo, rel)
            domains = {h.split("/")[1] for h in (info["declared"] | info["incl_headers"])}
            if len(domains) >= 3:
                counts[rel] = len(domains)
    if len(counts) > CATCH_ALL_BASELINE:
        print(f"[WARN] TEST-R10 趋势：跨 ≥3 模块域的测试文件 {len(counts)} 个，超出基线 {CATCH_ALL_BASELINE}"
              f"（catch-all 反模式抬头）：")
        for rel, domain_count in sorted(counts.items()):
            print(f"  {rel}: {domain_count} 域")
    else:
        print(f"[INFO] TEST-R10 趋势：跨 ≥3 模块域测试文件 {len(counts)}/{CATCH_ALL_BASELINE}（基线内）。")


def main() -> int:
    repo = sys.argv[1] if len(sys.argv) > 1 else repo_root_of(__file__)
    if not os.path.isdir(os.path.join(repo, "codespec")):
        print(f"[FAIL] not an Aurora repo root: {repo}")
        return 2

    problems = []
    check_doc_refs(repo, problems)
    check_test_headers(repo, problems)
    check_public_header_coverage(repo, problems)
    check_parallel_safety(repo, problems)
    check_test_naming(repo, problems)
    check_using_directive(repo, problems)
    report_catch_all_trend(repo)

    remaining = []
    r5_reported = []
    for rule, rel, lineno, detail in problems:
        rel = rel.replace("\\", "/")
        if (rule, rel, detail) in WHITELIST:
            continue
        if rule == "R5" and not TEST_R5_ENFORCED:
            r5_reported.append((rule, rel, lineno, detail))
            continue
        remaining.append((rule, rel, lineno, detail))

    if r5_reported:
        print(f"[WARN] TEST-R5 降级中（测试体系重写期间）：{len(r5_reported)} 个公共头暂无测试覆盖，"
              f"仅报告、不判失败。")

    if not remaining:
        print("[OK] code-doc sync clean: 0 problems.")
        return 0

    print(f"[FAIL] code-doc sync: {len(remaining)} problem(s):")
    by_rule = {}
    for rule, _rel, _lineno, _detail in remaining:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    for rule in sorted(by_rule):
        print(f"  {rule}: {by_rule[rule]}")
    print("")
    for rule, rel, lineno, detail in remaining[:80]:
        print(f"  {rule} {rel}:{lineno}  {detail}")
    if len(remaining) > 80:
        print(f"  ... and {len(remaining) - 80} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
