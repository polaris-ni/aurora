#!/usr/bin/env python3
# ============================================================================
# check_doc_comments.py - Doxygen 文档注释规范门禁
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §13（Doxygen 注释规范）。规则编号 DOC-R1—DOC-R8。
#
# 为什么需要独立门禁（clang-format / clang-tidy 都不会报）：
#   Doxygen 的文档标记是**多选一**形态（`/** */`、`/*! */`、`///`、`//!`、尾注 `///<` 与
#   `/**<`，命令前缀 `@cmd` 与 `\cmd`），任何一种都能出文档，工具链因此不会拒绝混用。
#   混用的代价要事后才显现：按标记检索注释的自动化（文档生成、AI 面投影、注释体检）
#   在不同目录得到不同结果；块注释的 ` * ` 续行缩进由作者与 clang-format 两边争夺，反复漂移。
#   本门禁把形态钉死为单一写法，并把「注释与声明的归属关系」变成机器可判的相邻性。
#
# Scan points（受版控 C/C++ 源文件：include/ src/ tools/ tests/ examples/，third_party 除外）:
#   1) [DOC-R1] 标记唯一：文档注释只能用 ///，成员尾注只能用 ///<；禁止 /** */、/*! */、
#      //!、/**< */ 形态。
#   2) [DOC-R2] 命令前缀唯一：禁止 \command，统一 @command。
#   3) [DOC-R3] 界限正向：/// 块必须紧邻可文档化声明（类型 / 变量 / 函数 / 宏 / 命名空间 /
#      @file），否则它是实现叙述，须降级为 //。
#   4) [DOC-R4] 界限反向：include/ 里同缩进、无空行紧邻公共声明的 // 散文注释，就是该声明的
#      文档，须升级为 ///。
#   5) [DOC-R5] 完整度：include/ 的可文档化声明必须有 /// 块，块内必须有 @brief；函数还须为
#      每个具名形参写 @param，为非 void 返回写 @return。
#   6) [DOC-R6] 排版：@brief 必须在块首行；一旦出现命令行，其后不得**另起散文段落**
#      （空 /// 行 + 散文）。命令行描述的跨行续写紧贴上一行，属合法形态。
#   7) [DOC-R7] 命令行取值下限：需要参数的命令不得空写（@example 无文件名、@param/@tparam/
#      @retval 无名或名字不在签名形参表里、@brief/@return/@note/@warning/@see/@details 无文字），
#      @deprecated 须在同一行给出替代路径与生效版本；void 函数与构造/析构不得写 @return。
#      理由：这类写法在 Doxygen 里不是「信息少」而是「语义换人」——空的 @example 会把**当前
#      头文件**登记成示例源，与文件级注释块相撞，而 Doxyfile 开着 WARN_AS_ERROR，一条空
#      @example 就能让文档站构建中止；@param 名对不上签名则该形参在文档站里仍是未文档化。
#   8) [DOC-R8] 尾注只挂真实成员：函数体内的语句与局部声明不得用 ///<（它们不是成员、不进文档）。
#      这条专治「为消门禁给语句补 ///<」——真成员缺口被噪音盖住，读者误以为语句是 API 的一部分。
#
# 不在本门禁范围内的三件事：
#   * §5 的 @note Thread / Side-effects / Rebuildable 契约标注取值（由契约门禁负责）。
#   * 注释内容质量（措辞是否说清语义）——机器只判形态、归属、齐全度与命令取值下限（DOC-R7
#     只问「参数在不在」，不问「参数写得对不对」）。
#   * 字面量里的 `//` 与 `@group`/`@binding`（WGSL/GLSL 源码）：扫描前先把字符串、字符与
#     原始串内容掩成空格，故不会污染判定。
#
# 用法：python tools/check/check_doc_comments.py [--root .] [--rules DOC-R1,DOC-R5]
#       [--files include/aurora/core] [--report out.json] [--limit N]
# 退出码：0 无违规；1 有违规；2 CLI / 读取错误。
# ============================================================================
"""Doxygen 文档注释规范门禁：标记形态、// 与 /// 的归属界限、@brief/@param/@return 齐全度。"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

SOURCE_EXTS = (".h", ".hpp", ".hh", ".hxx", ".inl", ".cpp", ".cc", ".cxx")
DEFAULT_ROOTS = ("include", "src", "tools", "tests", "examples")
SKIP_DIRS = {"third_party", ".git"}
SKIP_DIR_PREFIX = "build"

CMD_RE = re.compile(r"@\w+")
BLOCK_CMD_RE = re.compile(r"^@(param|return|retval|note|warning|tparam|throws|exception|deprecated|see|details)\b")
# DOC-R7：命令 → 该命令缺失参数时的说明。名字类命令先剥掉 [in] 方向与 <...> 约束再取名。
NAME_ARG_CMDS = {"param": "parameter name", "tparam": "template parameter name",
                 "retval": "return value name"}
TEXT_ARG_CMDS = {"brief": "one-line description", "example": "sample source file name",
                 "return": "return value description",
                 "note": "content", "warning": "content",
                 "see": "reference target", "details": "detailed text"}
# @deprecated 的附加义务（§13.5.2）：替代路径与生效版本都得在同一行出现。
DEPRECATED_PATH_RE = re.compile(r"改用|替换为|请用|改为|等价于|replac|\buse\s")  # CJK-LITERAL: regex-semantic
DEPRECATED_VERSION_RE = re.compile(r"\d+\.\d+|\bsince\b|\bv\d|自\s*\d|起于\s*\d")  # CJK-LITERAL: regex-semantic
BACKSLASH_RE = re.compile(r"^\\(brief|param|return|retval|note|tparam|see|code|endcode|warning|todo|details|ingroup|defgroup|addtogroup|deprecated|author|date|version|throws|exception)\b")
BANNER_RE = re.compile(r"^[\s\-*=#_+·─━▌/.|]*$")
COND_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|else|elif|endif|pragma|include)\b")
TYPE_RE = re.compile(r"^(?:template\s*<.*?>\s*)?(?:class|struct|union|enum|namespace|concept)\b")
CONCEPT_RE = re.compile(r"^(?:template\s*<.*?>\s*)?concept\b")
NAMESPACE_RE = re.compile(r"^(?:namespace\b|inline namespace\b)")
FUNC_RE = re.compile(r"^(?:template\s*<.*?>\s*)?(?:(?:explicit|static|inline|constexpr|virtual|friend|friend\s+static|auto|co_return)\s+)*[\w:<>~,\s\*&\[\]\.]*[\w~]+\s*(?:<.*?>\s*)?\(")
OPER_RE = re.compile(r"^(?:template\s*<.*?>\s*)?[\w:<>,\s\*&\[\]]*\boperator\b[\w:+\-*/<>=!%&|^~()\[\]{}\s]*\(")
# `const` / `volatile` 既是类型限定符也可能出现在指针形态的「类型 *名字」之间（`const char *k_x = "…"`），
# 故两侧都要容忍：前缀循环收一次，名字前的可选 `[*&]+` 组再收一次，否则整条常量声明对门禁不可见，
# 其上方的文档块只会被判成孤立 ///（DOC-R3）。
VAR_RE = re.compile(r"^(?:(?:static|constexpr|inline|extern|thread_local|mutable|const|volatile)\s+)*"
                    r"[A-Za-z_][\w:]*(?:<[^;{}]*>)?(?:\s*[*&]+\s*|\s+)(?:\s*[*&]+\s*)?[A-Za-z_]\w*"
                    r"(?:\s*\[\s*\d*\s*\])?\s*(?:=\s*\{?|;|\{)")
MACRO_RE = re.compile(r"^#\s*define\s+([A-Za-z_]\w*)(\()?")
ALIAS_RE = re.compile(r"^(?:template\s*<[^>]*>\s*)?(?:using|typedef)\b")
ENUM_RE = re.compile(r"^(?:template\s*<.*?>\s*)?enum\b")
ENUM_ITEM_RE = re.compile(r"^([A-Za-z_]\w*)\s*(?:=\s*[^;{}]+)?$")
CPP_KEYWORDS = {"template", "typename", "class", "struct", "enum", "const", "constexpr", "auto", "void", "namespace", "using", "static", "inline", "unsigned", "signed", "int", "bool", "char", "float", "double", "size_t", "operator", "return", "noexcept", "requires"}


def template_params(decl: str) -> list[str]:
    """取声明头部第一层 template<...> 里的具名形参（无名形参与约束占位返回空）。"""
    m = re.match(r"\s*template\s*<", decl)
    if not m:
        return []
    open_at = m.end() - 1
    depth, close_at = 0, -1
    for i in range(open_at, len(decl)):
        if decl[i] == "<":
            depth += 1
        elif decl[i] == ">":
            depth -= 1
            if depth == 0:
                close_at = i
                break
    if close_at < 0:
        return []
    params, buf, nest = [], [], 0
    for ch in decl[open_at + 1:close_at]:
        if ch in "<({":
            nest += 1
        elif ch in ">)}":
            nest -= 1
        if ch == "," and nest == 0:
            buf.append("")
            continue
        if not buf:
            buf.append(ch)
        else:
            buf[-1] += ch
    out = []
    for raw in buf:
        part = raw.strip()
        if not part or "template" in part:
            continue
        part = re.split(r"=(?!=)", part, maxsplit=1)[0].strip()
        ids = [t for t in re.findall(r"[A-Za-z_]\w*", part) if t not in CPP_KEYWORDS]
        if ids:
            out.append(ids[-1])
    return out


def has_param(text: str, name: str) -> bool:
    """注释块里是否为该形参写了 @param（`@param 名称` / `@param[in] 名称` / `@param[in,out] 名称` 均可）。"""
    pat = r"@param\b(?:\s*\[[^\]]*\])*\s*" + re.escape(name) + r"\b"
    return bool(re.search(pat, text))


def has_tparam(text: str, name: str) -> bool:
    """注释块里是否为该模板形参写了 @tparam。"""
    pat = r"@tparam\b(?:\s*\[[^\]]*\])*\s*" + re.escape(name) + r"\b"
    return bool(re.search(pat, text))


def _doc_cmd_names(text: str, cmd: str, dedupe: bool = True) -> list[str]:
    """取出注释块里某命令写出的第一个标识符（即形参名 / 模板形参名），按书写顺序排列。

    名字缺失（`@param` 空写）时返回空：紧随的下一行以 `///` 开头，而 `/` 不在 `\\s` 里，
    因此跨行续写的描述不会被误当成名字。

    `dedupe=False` 保留重复项——判「同一形参写了两条 @param」必须看得见第二次，Doxygen 对
    这种形态报 `has multiple @param documentation sections`。
    """
    out: list[str] = []
    for m in re.finditer(r"@" + cmd + r"\b(?:\s*\[[^\]]*\])*\s*([A-Za-z_]\w*)?", text):
        name = m.group(1)
        if not name:
            continue
        if dedupe and name in out:
            continue
        out.append(name)
    return out


def documented_params(text: str, dedupe: bool = True) -> list[str]:
    """注释块里 @param 写出的形参名。"""
    return _doc_cmd_names(text, "param", dedupe)


def documented_tparams(text: str, dedupe: bool = True) -> list[str]:
    """注释块里 @tparam 写出的模板形参名。"""
    return _doc_cmd_names(text, "tparam", dedupe)

STATEMENT_RE = re.compile(
    r"^(?:if|for|while|switch|return|break|continue|do|else|case|default|try|catch|throw|goto|"
    r"static_assert|using\s+namespace|AURORA_[A-Z_]+|CO_\w+)\b")
ATTR_RE = re.compile(r"^\[\[[a-z_]+(?:\([^\]]*\))?\]\]\s*")
# 仓库自定义的标注宏（`AURORA_MAIN_THREAD` 展开成 `[[clang::annotate(...)]]`）写在声明最前面，
# 不剥掉的话 FUNC_RE / VAR_RE 一律匹配不上，整条声明对门禁「不可见」，其文档块只会被判成孤立 ///。
MACRO_ATTR_RE = re.compile(r"^AURORA_[A-Z0-9_]+\s+")
ACCESS_RE = re.compile(r"^(public|private|protected)\s*:(?P<rest>.*)$")
TPL_HDR_RE = re.compile(r"^template\s*<")
# 独占一行的约束子句：`template <class T>` 换行后写 `requires …`，再下一行才是真正的声明。
# 它自己不是声明体，判定「注释块是否紧邻声明」时必须与模板头一样被跨过，否则紧邻的声明看不见。
REQUIRES_HDR_RE = re.compile(r"^requires\b")
# 内部实现命名空间（§13.5.1）：`detail` / `internal` / `impl` 里的声明不是公共 API 面，
# 热路径性能与实现理由用 `//` 叙述即可，门禁不索要文档注释（Doxygen 也不把它们投影给消费者）。
NS_NAME_RE = re.compile(r"^(?:inline\s+)?namespace\s+(?P<name>[\w:]+)")
INTERNAL_NS = {"detail", "internal", "impl"}
FWD_DECL_RE = re.compile(r"^(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+\w+\s*;$")
FRIEND_RE = re.compile(r"^\s*friend\b")
# 类外**限定定义**（`auto Preferences::get_impl(...) -> T {`）：契约文档属于类内的声明处。
# 若再要求定义处补一份 /// 块，Doxygen 侧会变成同一成员双份文档，且私有实现成员（`*_impl`）
# 会被迫写出「公共文档」——§13.5.1 的豁免在类外定义形态上无法从局部文本判出。
QUALIFIED_RE = re.compile(r"^(?:.*[^\w:])?([A-Za-z_]\w*)::"
                          r"(?:operator\b|[~A-Za-z_]\w*(?:<[^>]*>)?\s*(?:\(|$|\bconst\b|->|=|\{))")


def is_qualified_definition(decl: str) -> bool:
    """是否为**类外限定定义**（`auto Preferences::get_impl(...) -> T {`）。

    判定必须只看第一个圆括号之前的头部：`auto to_json(...) -> std::string;` 的 `std::string`
    是返回类型，按整串匹配 `::` 会被误判成限定定义，从而让该函数逃掉整套齐全度检查。
    """
    body = strip_attrs(decl.strip())
    cp = call_paren(body)
    head = body if cp < 0 else body[:cp]
    return bool(QUALIFIED_RE.match(head.rstrip()))

PRIMITIVE_WORDS = {"const", "constexpr", "unsigned", "signed", "struct", "class", "enum", "void",
                   "auto", "int", "bool", "char", "double", "float", "long", "short", "size_t",
                   "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t",
                   "int64_t", "string", "size_type", "restrict", "volatile"}


def mask_literals(text: str) -> str:
    """把字符串 / 字符 / 原始串字面量内容替换为空格（保留换行），使扫描只看真注释。

    注释必须在本函数里就地跳过而不是事后识别：注释文本里的引号（`/// 所谓 "the true cost"`、
    `// don't`）一旦按字面量起始处理，掩码会把从该引号到**下一处配对引号**之间的所有行抹平，
    包括中间那些 `///` 标记本身——于是连续的文档注释块被拦腰截断，门禁随即误报 DOC-R3 / DOC-R6。
    单趟从左到右扫描即可判定「先遇到的是注释起始还是字面量起始」，`"https://x"` 这类串内的 `//`
    仍按串处理（先命中引号），不会误当注释。
    """
    out = list(text)
    i, n = 0, len(text)

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        ch = out[i]
        if ch == "/" and i + 1 < n and out[i + 1] == "/":
            nl = text.find("\n", i)
            i = n if nl < 0 else nl
            continue
        if ch == "/" and i + 1 < n and out[i + 1] == "*":
            close = text.find("*/", i + 2)
            i = n if close < 0 else close + 2
            continue
        if ch == "R" and i + 1 < n and out[i + 1] == '"':
            m = re.match(r'R"([A-Za-z0-9_]{0,16})\(', text[i:])
            if m:
                close = ')"' + m.group(1)
                end = text.find(close, i + m.end())
                stop = n if end < 0 else end + len(close)
                blank(i, stop)
                i = stop
                continue
        if ch in ('"', "'"):
            quote, k = ch, i + 1
            closed = False
            while k < n:
                if text[k] == "\\":
                    k += 2
                    continue
                if text[k] == quote:
                    k += 1
                    closed = True
                    break
                if quote == "'" and text[k] == "\n":
                    break
                k += 1
            # 数字分隔符（`1'000'000`、`60'000.0`）里的 `'` 不是字符字面量：行内配不到对引号就原样保留。
            # 若照旧抹到行尾，其后的真代码（包括 `}`）会一起消失，花括号配对错位，函数体区间一路吞到文件末尾。
            if quote == "'" and not closed:
                i += 1
                continue
            blank(i, min(k, n))
            i = k
            continue
        i += 1
    return "".join(out)


def scan(masked: str) -> list[dict]:
    """扫描掩码后的文本，产出注释片段。kind: doc | plain | block（含 blockdoc）。

    code_before 表示该行注释之前**有真代码**（缩进不算），即尾注形态。
    """
    notes: list[dict] = []
    line, col, i, n = 1, 0, 0, len(masked)
    line_start = 0
    while i < n:
        ch = masked[i]
        if ch == "\n":
            line += 1
            col = 0
            i += 1
            line_start = i
            continue
        col += 1
        if ch == "/" and i + 1 < n:
            nxt = masked[i + 1]
            if nxt == "/":
                end = masked.find("\n", i)
                end = n if end < 0 else end
                raw = masked[i:end]
                if raw.startswith("////"):
                    kind = "plain"
                elif raw.startswith("///"):
                    kind = "doc"
                elif raw.startswith("//!"):
                    kind = "doc_bad"
                else:
                    kind = "plain"
                notes.append({"kind": kind, "line": line, "col": col, "end_line": line,
                              "text": raw, "code_before": bool(masked[line_start:i].strip())})
                col += end - i
                i = end
                continue
            if nxt == "*":
                end = masked.find("*/", i + 2)
                end = n if end < 0 else end + 2
                block = masked[i:end]
                notes.append({"kind": "block", "blockdoc": bool(re.match(r"^/\*[*!]", block)) or "<" in block[:4],
                              "line": line, "col": col, "end_line": line + block.count("\n"),
                              "text": block, "code_before": bool(masked[line_start:i].strip())})
                line += block.count("\n")
                col = 0
                i = end
                if line_start < i:
                    line_start = masked.rfind("\n", 0, i) + 1
                continue
        i += 1
    return notes


class Source:
    """单个源文件的注释 / 声明视图。行号一律 1 基。"""

    def __init__(self, rel: str, text: str) -> None:
        self.rel = rel
        self.lines = text.splitlines()
        self.masked = scan(mask_literals(text))
        self.masked_lines = mask_literals(text).split("\n")
        self.comment_lines: dict[int, dict] = {}
        for note in self.masked:
            for ln in range(note["line"], note["end_line"] + 1):
                self.comment_lines.setdefault(ln, note)
        self.code_lines = self._strip_comments()

    def _strip_comments(self) -> list[str]:
        """掩掉注释后的纯代码行（与 masked_lines 行数一一对应）。

        花括号配对必须用它：注释文本里的 `{` / `}` 不是真括号，按 masked_lines 计数会凭空造出
        「函数体区间」，让体内根本没有语句的行被判成 R8。
        """
        out = list(self.masked_lines)
        for note in self.masked:
            a, b = note["line"], note["end_line"]
            if a > len(out):
                continue
            cut = note["col"] - 1
            if a == b:
                seg = out[a - 1]
                # 行中间的块注释（`f(/*ctx*/ int x)`）只吃掉注释本身：整行截断会把注释之后的真代码
                # （形参表、行尾 `{`）一起丢掉，函数体闭括号提前弹出，作用域栈整个错位。
                k = seg.find("*/", cut) if note["kind"] == "block" else -1
                if k >= 0:
                    tail = seg[k + 2:]
                    out[a - 1] = seg[:cut] + " " * (len(seg) - cut - len(tail)) + tail
                else:
                    out[a - 1] = seg[:cut]
                continue
            out[a - 1] = out[a - 1][:cut]
            for ln in range(a + 1, min(b, len(out))):
                out[ln - 1] = ""
            last = min(b, len(out))
            tail = out[last - 1]
            k = tail.find("*/")
            out[last - 1] = " " * (k + 2) if k >= 0 else ""
        return out

    def note_at(self, line: int) -> dict | None:
        return self.comment_lines.get(line)

    def is_blank_code(self, idx: int) -> bool:
        """0 基行：掩码后为空，或整行都在注释里。"""
        if idx >= len(self.lines):
            return True
        return not self.masked_lines[idx].strip()

    def next_code(self, after: int) -> int | None:
        """返回 after（1 基）之后第一个非空非纯注释行的 0 基下标。"""
        idx = after
        while idx < len(self.lines):
            line = self.masked_lines[idx]
            if line.strip():
                return idx
            idx += 1
        return None

    def prev_comment_run(self, decl_idx: int) -> tuple[str, int, int, str] | None:
        """紧邻声明上方（同缩进、无空行）的注释块：返回 (kind, 起始行, 结束行, 文本)。

        允许声明与注释块之间隔着条件编译行（`#ifdef` / `#endif` 等），因为受开关保护的
        公共 API 常把文档写在 `#if` 之上。
        """
        probe = decl_idx - 1
        hops = 0
        while probe >= 0 and self.lines[probe].strip() == "":
            probe -= 1
            break
        while probe >= 0 and COND_RE.match(self.lines[probe]) and hops < 2:
            probe -= 1
            hops += 1
            while probe >= 0 and self.lines[probe].strip() == "":
                probe -= 1
        if probe < 0 or not self.masked_lines[probe].lstrip().startswith(("//", "/*", "*")):
            return None
        note = self.note_at(probe + 1)
        if not note or note["code_before"]:
            return None
        if note["kind"] == "block":
            kind = "blockdoc" if note.get("blockdoc") else "block_plain"
        elif note["text"].startswith("/*"):
            kind = "block_plain"
        else:
            kind = note["kind"]
        end = note["end_line"]
        start = note["line"]
        indent = len(self.lines[probe]) - len(self.lines[probe].lstrip())
        while start - 2 >= 0:
            prev = self.note_at(start - 1)
            if not prev or prev["code_before"] or prev["end_line"] != prev["line"]:
                break
            if prev["kind"] != note["kind"]:
                break
            if len(self.lines[start - 2]) - len(self.lines[start - 2].lstrip()) != indent:
                break
            start -= 1
        return kind, start, end, "\n".join(self.lines[start - 1:end])

    def declarations(self):
        """产出 (decl_idx0, 声明文本, 结束行, 访问域)。函数体内的语句被跳过。

        访问域由**作用域栈**跟踪：进入类型 / 命名空间体时压栈，花括号深度回落到该体之下时出栈，
        恢复外层作用域记录的访问域。早先只存一个扁平 access、见到 `};` 就无条件复位 public，
        于是类内嵌套的 `enum class` / `struct` / `friend class` 定义一收尾，外层类的
        private / protected 成员就被当成公共 API 要求文档注释（§13.5.1 明确豁免它们）。
        """
        idx, n = 0, len(self.lines)
        depth = 0
        stack: list[list] = []  # 每项 [进入该体后的深度, 该体内的当前访问域]
        tpl: list[str] = []     # 连续独占行的模板头，等下一行的声明体
        tpl_start = 0

        def braces(a: int, b: int) -> int:
            return sum(self.code_lines[j].count("{") - self.code_lines[j].count("}")
                       for j in range(a, min(b, n)))

        def settle(to: int) -> None:
            nonlocal depth
            depth = to
            while stack and depth < stack[-1][0]:
                stack.pop()

        while idx < n:
            if self.is_blank_code(idx):
                idx += 1
                continue
            if idx > 0 and self.lines[idx - 1].rstrip().endswith("\\"):
                # 预处理续行的后半段（`#if defined(A) || \` 的下一行）不是声明起始：`defined(` 会被
                # FUNC_RE 认成函数名，于是 fold_span 把整段条件编译并成一条「声明」，向其索要文档。
                idx += 1
                continue
            acc_marker = ACCESS_RE.match(self.lines[idx].strip())
            if acc_marker:
                if stack:
                    stack[-1][1] = acc_marker.group(1)
                idx += 1
                continue
            raw = self.lines[idx].strip()
            if (TPL_HDR_RE.match(raw) or (tpl and REQUIRES_HDR_RE.match(raw))) and \
                    fold(self.lines, self.masked_lines, idx)[0] is None:
                # 独占一行的模板头（与紧随其后的约束子句）：Doxygen 认可「注释块在 template 之上」这一
                # 标准形态，注释属于下一行的声明。早先只按行折叠，此类注释会被判成孤立 ///（R3），
                # 并把 template 行挤到注释块上方去规避——那反而让 @tparam 失去绑定对象。
                tpl_start = tpl_start if tpl else idx
                tpl.append(raw)
                idx += 1
                continue
            text, end = fold(self.lines, self.masked_lines, idx)
            if text is None:
                nxt = balance_forward(self.code_lines, idx, n, skip_open=True)
                settle(depth + braces(idx, nxt))
                idx = max(nxt, idx + 1)
                tpl = []
                continue
            if tpl:
                text = " ".join(tpl + [text])
                idx = tpl_start
                tpl = []
            access = stack[-1][1] if stack else "public"
            if TYPE_RE.match(text):
                inner = "private" if re.match(r"^\s*(?:template\s*<[^>]*>\s*)?class\b", text) else "public"
                ns = NS_NAME_RE.match(text)
                if ns and any(seg in INTERNAL_NS for seg in ns.group("name").split("::")):
                    inner = "internal"      # `namespace detail {` 整段作用域都不算公共 API 面（§13.5.1）
                if access != "public":
                    # 外层访问域是成员可见性的上界：私有 / 保护嵌套类型的成员对外同样不可达，
                    # 不该按公共 API 索要文档。
                    inner = access
                yield idx, text, end, access
                if CONCEPT_RE.match(text):
                    # concept 的 `requires { 表达式 }` 体里没有成员，只有探测用的伪声明：整体跳过。
                    nxt = balance_forward(self.code_lines, end, n, skip_open=True)
                    settle(depth + braces(idx, nxt))
                    idx = max(nxt, end + 1)
                    continue
                open_after = depth + braces(idx, end + 1)
                if open_after > depth:
                    stack.append([open_after, inner])
                settle(open_after)
                idx = end + 1
                continue
            yield idx, text, end, access
            if function_like(text):
                # 从**折叠结束行**起算：`{` 不在声明首行时（多行形参表 / template 头 + 下一行签名），
                # 从起始行起算会在首行就判出「深度已归零」并提前返回，函数体没被整体消费，
                # 体内局部声明与调用语句随即被逐行当成公共成员索要文档。
                nxt = balance_forward(self.code_lines, end, n, skip_open=True)
                settle(depth + braces(idx, nxt))
                idx = max(nxt, end + 1)
            else:
                settle(depth + braces(idx, end + 1))
                idx = end + 1

    def body_spans(self) -> list[tuple[int, int]]:
        """函数体覆盖的行区间（1 基闭区间）。体内语句与局部声明都不是可文档化成员。"""
        spans: list[tuple[int, int]] = []
        n = len(self.lines)
        for idx, decl, end, _access in self.declarations():
            if not function_like(decl) or not self.opens_block(self.code_lines, end):
                continue
            after = balance_forward(self.code_lines, end, n, skip_open=True)
            if after - 1 > end:
                spans.append((end + 2, after))
        return spans

    @staticmethod
    def opens_block(masked_lines: list[str], end: int) -> bool:
        seg = masked_lines[end]
        return seg.rstrip().endswith("{") or seg.count("{") > seg.count("}")


def function_like(decl: str) -> bool:
    body = strip_attrs(decl.strip())
    return call_paren(first_member(body)) >= 0 and bool(OPER_RE.match(body) or FUNC_RE.match(body))


def top_level_commas(code: str) -> int:
    """统计不在 `<...>` / `(...)` / `{...}` 内的逗号数 + 1，即该行声明的个数。

    多声明共用一条尾注时，Doxygen 只把注释给其中**一个**声明，其余在文档站里静默失踪；
    括号内的逗号（模板实参、形参表、聚合初始化）不算独立声明，故按深度过滤。
    """
    count, depth = 0, 0
    code = code.rstrip().rstrip(",")  # 枚举项 / 声明的尾逗号后面没有第二个声明，不算多声明
    for ch in code:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth = max(0, depth - 1)
        elif ch == "," and depth == 0:
            count += 1
    return count + 1


def is_function_pointer(code: str) -> bool:
    """判断是否为「函数指针 / std::function 之类可调用对象」的数据成员声明。

    这类行含括号，按 `call_paren` 会被认成函数，于是它的 ///< 尾注被误判成「函数用了尾注」。
    真函数的形参表总在尖括号之外以 `(` 开头且紧跟形参；函数指针的声明符是 `(*name)(...)`，
    模板类型（`std::function<void(int)>`）的括号则全部包在 `<...>` 里。
    """
    angle, idx = 0, 0
    while idx < len(code):
        ch = code[idx]
        if ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "(" and angle == 0:
            rest = code[idx + 1:].lstrip()
            return not rest or rest.startswith(("*", "&"))
        idx += 1
    return True


def looks_like_function(code: str) -> bool:
    """可文档化「函数形态」判据：尖括号之外有形参表、且声明符不是函数指针。"""
    return not is_function_pointer(code)


def has_value_initializer(code: str) -> bool:
    """声明符后是否跟着「值初始化器」——尖括号/括号之外的 `=`，且右端不是 `default`/`delete`。

    `std::shared_ptr<X> p = std::make_shared<X>();` 里的 `()` 属于初始化表达式，按「尖括号外有 `(`」
    判函数形态就会把这条数据成员认成函数，进而向它的 ///< 尾注索要 @param/@return（DOC-R5/DOC-R8
    双向误报）。`= default` / `= delete` 则确实是特殊成员（函数形态），不算值初始化器。
    """
    angle = paren = brace = 0
    for idx, ch in enumerate(code):
        if ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == "{":
            brace += 1
        elif ch == "}":
            brace = max(0, brace - 1)
        elif ch == "=" and angle == 0 and paren == 0 and brace == 0:
            prev = code[idx - 1] if idx else ""
            # `operator=` 里的 `=` 是运算符名的一部分，写在括号之前，不是初始化器；
            # 漏掉这一条会把每个 `auto operator=(…) -> X &` 判成数据成员，于是该声明的
            # 形参表 / @param / @return 三面判据一起被跳过（Doxygen 却照样抽取并报悬名 @param）。
            before = code[:idx].rstrip()
            if before.endswith("operator") and not (len(before) > 8 and (before[-9].isalnum() or before[-9] == "_")):
                continue
            if prev in "!<>=+-*/%&|^" or code[idx + 1:idx + 2] == "=":
                continue
            rest = code[idx + 1:].lstrip()
            if not rest.startswith(("default", "delete")):
                return True
    return False


def strip_attrs(body: str) -> str:
    while True:
        m = ATTR_RE.match(body) or MACRO_ATTR_RE.match(body)
        if not m:
            return body
        body = body[m.end():]


def balance_forward(masked_lines: list[str], start: int, n: int, skip_open: bool = False) -> int:
    """从 start 起累计花括号，返回第一个使平衡归零的行之后一行；无花括号则返回 start+1。"""
    depth = 0
    for j in range(start, min(start + 4000, n)):
        seg = masked_lines[j]
        depth += seg.count("{") - seg.count("}")
        if depth > 0:
            continue
        if depth < 0:
            return start + 1
        if skip_open and j == start:
            return start + 1
        return j + 1
    return start + 1


def fold(lines: list[str], masked_lines: list[str], idx: int) -> tuple[str | None, int]:
    """若第 idx 行（0 基）是可文档化声明的起始行，返回 (折叠声明文本, 结束行)。"""
    raw = lines[idx].strip()
    if raw.startswith("#"):
        return (re.sub(r"\s+", " ", raw), idx) if MACRO_RE.match(raw) else (None, idx)
    body = strip_attrs(raw)
    if not body or body.startswith(("*", "//", "public:", "private:", "protected:", "}", "{", ":")):
        return None, idx
    # 独占一行的约束子句（`template <class T>` 之后的 `requires …`）不是声明体：它归上一行的模板头
    # 管，由 declarations() / 紧邻探测逐行跨过。
    if REQUIRES_HDR_RE.match(body):
        return None, idx
    if STATEMENT_RE.match(body):
        return None, idx
    is_type = bool(TYPE_RE.match(body))
    is_alias = bool(re.match(r"^(?:template\s*<[^>]*>\s*)?(?:using|typedef)\b", body))
    is_func = bool(OPER_RE.match(body) or FUNC_RE.match(body))
    is_var = bool(VAR_RE.match(body))
    if not (is_type or is_alias or is_func or is_var):
        return None, idx
    if is_func or is_var:
        joined, end = fold_span(masked_lines, idx)
    else:
        # 类型/别名只取本行；必须先用 masked_lines 砍掉行尾注释，否则 `// NOLINT(*-enum-size)` 里的
        # 括号会被 parse_params 当成参数表，凭空捏造出一个「缺 @param: size」。
        seg = masked_lines[idx].split("//")[0]
        joined, end = strip_attrs(re.sub(r"\s+", " ", seg)), idx
    return joined.strip(), end


def fold_span(masked_lines: list[str], idx: int) -> tuple[str, int]:
    """把跨行声明折叠为一行，直到括号平衡且以 ; / { / 收尾花括号结束（最多 12 行）。

    单行成员定义（`void f() {}`、`auto g() -> int { return 1; }`）必须以 `}` 收尾即停：
    早先只认 `;` 与 `{`，这类行会继续往下并，把**下一个**成员甚至整个函数体并进同一条声明串，
    于是体内语句被当成公共数据成员报缺文档。`{` 收尾（类/命名空间/多行函数体的开句）照旧立即停。
    """
    parts: list[str] = []
    depth = 0
    braces = 0
    for j in range(idx, min(idx + 12, len(masked_lines))):
        seg = masked_lines[j]
        cut = seg.find("//")
        if cut >= 0:
            seg = seg[:cut]
        depth += seg.count("(") - seg.count(")") + seg.count("[") - seg.count("]")
        braces += seg.count("{") - seg.count("}")
        parts.append(seg.strip())
        joined = " ".join(parts).strip()
        if depth > 0:
            continue
        if joined.endswith((";", "{", "const", "noexcept", "override", "const {")):
            return joined, j
        if joined.endswith(("}", "} const", "} noexcept")) and braces <= 0:
            return joined, j
    return " ".join(parts).strip(), min(idx + 11, len(masked_lines) - 1)


def first_member(decl: str) -> str:
    """截出声明串里的第一个成员：到它的函数体结束或 `;` 为止。

    `fold` 会把相邻的单行成员定义并成一条声明串（`auto a() -> int { … } auto b() -> void { … }`），
    此时整串的返回类型判定会被后一个成员污染。凡按「这一个成员」判定的规则都先走这里。
    """
    depth, opened = 0, False
    for i, ch in enumerate(decl):
        if ch == "{":
            depth += 1
            opened = True
        elif ch == "}":
            depth -= 1
            if opened and depth == 0:
                return decl[:i + 1]
        elif ch == ";" and depth == 0:
            return decl[:i + 1]
    return decl


def call_paren(decl: str) -> int:
    """返回第一个**不在模板实参 `<>` 内**的 `(` 下标；没有则 -1（该声明不是可调用签名）。

    `std::function<void(int)> on_click;` 这类回调**数据成员**的括号写在 `<...>` 里，早先直接按
    正则抓 `void(` 就把它当函数，于是向数据成员要求 `@param int` 与 `@return`。
    `operator<` / `operator>()` 的符号本身也不算尖括号——遇 `operator` 先跳过其符号序列。
    """
    angle, i, n = 0, 0, len(decl)
    while i < n:
        if decl.startswith("operator", i) and (i == 0 or not (decl[i - 1].isalnum() or decl[i - 1] == "_")):
            j = i + 8
            while j < n and decl[j] not in "([ \t\n" and decl[j] in "+-*/%<>=!&|^~[]{}":
                j += 1
            i = j
            continue
        ch = decl[i]
        if ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "(" and angle == 0:
            return i
        i += 1
    return -1


def parse_params(decl: str) -> tuple[list[str], bool, bool]:
    """返回 (具名形参, 是否 void 返回, 是否构造/析构/操作符无需 return)。"""
    decl = strip_attrs(decl.strip())
    if call_paren(decl) < 0:
        return [], False, False
    m = re.search(r"([~\w:]+|\boperator\s*[\w:+\-*/<>=!%&|^~()\[\]{}\s]*?)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", decl)
    if not m:
        return [], False, False
    inner = m.group(2).strip()
    names: list[str] = []
    if inner and inner != "void":
        depth, chunk, parts = 0, "", []
        for ch in inner:
            if ch in "<([{":
                depth += 1
            elif ch in ">)]}":
                depth -= 1
            if ch == "," and depth == 0:
                parts.append(chunk)
                chunk = ""
            else:
                chunk += ch
        parts.append(chunk)
        for part in parts:
            # 行内块注释（`int /*n*/`）是给人类标的占位，不是形参名；Doxygen 也不为无名形参建文档。
            part = re.sub(r"/\*.*?\*/", " ", part).strip()
            if not part:
                continue
            head = re.split(r"=(?!=)", part, maxsplit=1)[0].strip()
            # 函数指针 / 引用形参（`void *(*proc)(const char *)`、`void (&cb)(int)`）：形参名写在
            # `(*name)` 里。按「最后一个非类型标识符」取会拿到内层签名的名字，真实形参反被误报成
            # 未文档化（Doxygen 侧同时报 @param 名不在形参表中）。
            fp = re.search(r"\(\s*[*&]+\s*([A-Za-z_]\w*)\s*\)", head)
            if fp:
                names.append(fp.group(1))
                continue
            # 具名形参的判据是**词法相邻**：名字前面必须是空白 / `*` / `&` / 省略号（`Args &&...args`
            # 的形参名写在 `...` 之后，Doxygen 同样把它算进形参表并索要 `@param args`），且后面到参数
            # 结尾（可跟数组下标）再无内容。按「最后一个非primitive标识符」取会把纯类型形参
            # （`const Ref&`、`std::string_view`）的类型名当成形参名，从而字面要求 `@param Ref`。
            mname = re.search(r"(?<=[\s*&.])([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", head)
            if mname and mname.group(1) not in PRIMITIVE_WORDS:
                names.append(mname.group(1))
    # `-> void` 才算无返回值；`-> void *`（裸指针句柄）与 `-> void_t` 都有返回值，
    # 用 `\\b` 会把它们一并当成 void，于是 @return 的必选/禁写两侧同时判错。
    void = bool(re.search(r"->\s*void\b(?![\w*&])", decl)) or bool(
        re.match(r"^(?:static\s+|inline\s+|constexpr\s+|virtual\s+)*void\s+\w", decl.strip()))
    # 构造/析构的判据是「调用括号之前的头部只有一个标识符」：`Name(…)`、`Cls::Name(…)`、`~Name(…)`。
    # 早先按「函数名与头部某个大写词同名」判定会误伤工厂函数（`Widget HSplitter(…)` 的名字恰与自身
    # 头部大写词重合），把合法的 `@return` 判成违规。宏（`#define FOO(…)`）头部含 `#`、operator 重载
    # 头部以 `operator` 结尾，二者都不满足本式，无需另作排除。
    cp = call_paren(decl)
    h = decl[:cp] if cp >= 0 else ""
    h = re.sub(r"^template\s*<.*?>\s*", "", h, flags=re.S)
    h = strip_attrs(h.strip())
    h = re.sub(r"^(?:(?:static|inline|constexpr|consteval|virtual|explicit|friend|mutable|"
               r"thread_local|extern)\s+)+", "", h.strip())
    if h.strip().startswith("requires"):
        # 文档块写在模板头之上时，约束子句也留在括号前的头部（`requires std::derived_from<…> Node`），
        # 整段文本自然匹配不上构造判据。名字取末尾标识符，但要先分辨它前面是什么：
        # 约束表达式总以闭合符（`>` / `)` / `]`）收尾，其后紧跟名字 ⇒ 没有返回类型 ⇒ 构造 / 析构；
        # 若前面是标识符（`auto` / `void` / 类型名）⇒ 那才是返回类型，本声明是普通函数。
        tail = re.search(r"(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*$", h.strip())
        if tail:
            before = h.strip()[:tail.start(1)].rstrip()
            h = tail.group(1) if (not before or before[-1] in ">)]") else before
    ctor = bool(re.match(r"^(?:[A-Za-z_]\w*(?:<[^>]*>)?::)*~?[A-Za-z_]\w*$", h.strip()))
    return names, void, ctor


class Checker:
    def __init__(self, root: str) -> None:
        self.root = root
        self.findings: list[dict] = []

    def add(self, rule: str, rel: str, line: int, msg: str) -> None:
        item = {"rule": rule, "file": rel, "line": line, "msg": msg}
        # 同一条判据可能从两个入口命中（如 check_layout 既按注释块、又按声明挂载点走一遍），
        # 重复条目会让「修好了还在报」的错觉进入报告与 JSON，修复方只能对着同一条反复核对。
        if item not in self.findings:
            self.findings.append(item)

    def check(self, path: str) -> None:
        rel = os.path.relpath(path, self.root).replace("\\", "/")
        try:
            text = open(path, "r", encoding="utf-8").read()
        except (OSError, UnicodeDecodeError) as exc:
            raise RuntimeError(f"unable to read {path}: {exc}") from exc
        src = Source(rel, text)
        public_api = rel.startswith("include/")
        before = len(self.findings)
        self.check_markers(src)
        self.check_blocks(src, public_api)
        self.check_body_notes(src)
        if public_api:
            self.check_declarations(src)
            self.check_matrix_extras(src)
        exempt = self.exempt_lines(src)
        self.findings[:] = [f for f in self.findings
                            if f["file"] != rel or f["line"] not in exempt or f["rule"] not in exempt[f["line"]]]

    @staticmethod
    def exempt_lines(src: Source) -> dict[int, set[str]]:
        """收集 `DOC-EXEMPT: <原因>` 放行的 (行号 → 规则集)。

        标记写在被放行行本身、或其上 2 行内即生效；`DOC-EXEMPT: *` 放行该行全部规则，
        `DOC-EXEMPT: DOC-R5` 只放行指定规则（与 HARDPATH_EXEMPT 同一形态，见 §10.5 第 10 条）。
        """
        out: dict[int, set[str]] = {}
        for idx, raw in enumerate(src.lines, start=1):
            m = re.search(r"DOC-EXEMPT:\s*(\S+)", raw)
            if not m:
                continue
            # 尾随的 `*/` 是行尾块注释的收尾，要剥掉；但单独的 `*`（放行全部规则）不能被 rstrip 剥成空串。
            token = m.group(1).strip()
            if token.endswith("*/"):
                token = token[:-2].strip()
            rules = {"DOC-R1", "DOC-R2", "DOC-R3", "DOC-R4", "DOC-R5", "DOC-R6",
                     "DOC-R7", "DOC-R8"} if token == "*" else {token}
            for target in range(idx, min(idx + 3, len(src.lines)) + 1):
                out.setdefault(target, set()).update(rules)
        return out

    # ---- DOC-R1 / DOC-R2 ----
    def check_markers(self, src: Source) -> None:
        for note in src.masked:
            if note["kind"] == "block" and note.get("blockdoc"):
                self.add("DOC-R1", src.rel, note["line"],
                         "block doc comment (/** */, /*! */, /**< */) must be changed to /// or ///<")
            elif note["kind"] == "doc_bad":
                self.add("DOC-R1", src.rel, note["line"], "//! doc comment must be changed to ///")
            elif note["kind"] == "doc":
                body = note["text"].lstrip("/").lstrip()
                if BACKSLASH_RE.match(body):
                    self.add("DOC-R2", src.rel, note["line"], r"command prefix \cmd must be changed to @cmd")
        for idx, raw in enumerate(src.lines, start=1):
            if "/**<" in raw or "/*!<" in raw:
                self.add("DOC-R1", src.rel, idx, "trailing note /**< */ must be changed to ///<")

    # ---- DOC-R3 / DOC-R6 ----
    def check_blocks(self, src: Source, public_api: bool) -> None:
        bodies = src.body_spans()
        for block in self.doc_blocks(src):
            start, end = block[0]["line"], block[-1]["line"]
            text = "\n".join(n["text"] for n in block)
            # 块夹在 template/requires 头与其声明之间：Doxygen 单文件抽取时看着像绑上了，一旦与同
            # 命名空间的其它头一起抽取就静默失去归属（§13.4）——注释不进文档站，也不报未文档化。
            # 模板头与它修饰的声明是同一个可文档化声明，块只能写在模板头之前。
            above = start - 2
            while above >= 0 and src.lines[above].strip() == "":
                above -= 1
            if above >= 0:
                prev_head = src.lines[above].strip()
                if TPL_HDR_RE.match(prev_head) or REQUIRES_HDR_RE.match(prev_head):
                    self.add("DOC-R3", src.rel, start,
                             "/// block sits between a template/requires header and its declaration; "
                             "move the whole block above the header")
                    continue
            nxt = src.next_code(end)
            attached = None
            pending: list[str] = []
            while nxt is not None:
                head = src.lines[nxt].strip()
                if COND_RE.match(head):
                    nxt = src.next_code(nxt + 1)
                    continue
                folded, _ = fold(src.lines, src.masked_lines, nxt)
                if folded is None and (TPL_HDR_RE.match(head) or (pending and REQUIRES_HDR_RE.match(head))):
                    # 独占一行的模板头（`template <class T>`）与紧随其后的约束子句（`requires …`）本身都不
                    # 是声明体：Doxygen 把写在它们上方的注释块绑定给下一行的类 / 函数，故逐行往下探，
                    # 并把这几行拼回声明文本，好让 @tparam 有对象可核对。
                    pending.append(head)
                    nxt = src.next_code(nxt + 1)
                    continue
                attached = (" ".join(pending + ([folded] if folded else []))) if folded else None
                break
            # 函数体内的语句 / 局部声明不是可文档化成员（§13.4 与 DOC-R8 同判据）：
            # `auto res = create_window(...)` 会被 VAR_RE 认成声明，于是贴在它上面的 /// 块看似「紧邻」
            # 而躲过 DOC-R3，实际是把 API 文档写进了实现体内，Doxygen 只会报 @param 与签名不符。
            if attached is not None and nxt is not None and any(a <= nxt + 1 <= b for a, b in bodies):
                self.add("DOC-R3", src.rel, start,
                         "/// block is inside a function body (next line is a statement or local "
                         "declaration); downgrade to //")
                continue
            if attached is None:
                if "@file" not in text:
                    self.add("DOC-R3", src.rel, start,
                             "/// block is not adjacent to a documentable declaration; implementation "
                             "narrative must become //")
                continue
            if not public_api:
                continue
            self.check_layout(src, block)
            self.check_command_usage(src, block)

    def doc_blocks(self, src: Source) -> list[list[dict]]:
        blocks: list[list[dict]] = []
        cur: list[dict] = []
        for note in src.masked:
            if note["kind"] in ("doc", "doc_bad") and not note["code_before"]:
                if cur and cur[-1]["end_line"] + 1 == note["line"] and cur[-1]["col"] == note["col"]:
                    cur.append(note)
                    continue
                if cur:
                    blocks.append(cur)
                cur = [note]
            elif cur:
                blocks.append(cur)
                cur = []
        if cur:
            blocks.append(cur)
        return blocks

    def check_layout(self, src: Source, block: list[dict]) -> None:
        """DOC-R6：@brief 首行；命令行之后不得**另起散文段落**。

        命令行的描述允许跨行续写（紧随其后的非空 /// 行按续读处理，clang-format 折行也落到
        这一形态），因此判据只能是「空 /// 行之后的散文段落」——插入空行即宣告新段落，
        而段落级散文排在命令之后就是排版错误（该当内容应前移到详述或并入 @note）。
        """
        bodies = [n["text"].lstrip("/").strip() for n in block]
        brief_idx = [i for i, b in enumerate(bodies) if b.startswith("@brief")]
        if brief_idx and brief_idx[0] != 0:
            self.add("DOC-R6", src.rel, block[brief_idx[0]]["line"], "@brief must be the first line of the doc comment")
        seen_cmd = False
        blank_after_cmd = False
        for i, note in enumerate(block):
            body = bodies[i]
            if BLOCK_CMD_RE.match(body):
                seen_cmd = True
                blank_after_cmd = False
            elif not body:
                blank_after_cmd = seen_cmd
            elif blank_after_cmd:
                self.add("DOC-R6", src.rel, note["line"],
                         "no prose paragraph may start after a command line (move it into @details "
                         "or @note; multi-line continuations must hug the previous line)")

    # ---- DOC-R7 ----
    def check_command_usage(self, src: Source, block: list[dict]) -> None:
        """DOC-R7：需要参数的命令不得空写；@deprecated 须给替代路径与生效版本。

        「命令名写了、参数没写」在 Doxygen 里不是信息缺失，而是语义换人：空的 @example 会把
        **当前头文件**登记成示例源，与它的文件级注释块相撞，Doxyfile 开着 WARN_AS_ERROR，
        一处空 @example 就能让整个文档站构建中止。命令行描述的跨行续写（§13.6）紧贴上一行，
        因此命令行为空但紧随一行散文时按续写处理，不算空写。
        """
        bodies = [n["text"].lstrip("/").strip() for n in block]
        for i, note in enumerate(block):
            m = re.match(r"^@([A-Za-z]+)\b(.*)$", bodies[i])
            if not m:
                continue
            cmd, rest = m.group(1), m.group(2).strip()
            if cmd == "deprecated":
                if not (DEPRECATED_PATH_RE.search(rest) and DEPRECATED_VERSION_RE.search(rest)):
                    self.add("DOC-R7", src.rel, note["line"],
                             "@deprecated must give the replacement path and the effective version "
                             "on the same line (Section 13.5.2)")
                continue
            if cmd in NAME_ARG_CMDS:
                rest = re.sub(r"^\[[^\]]*\]", "", rest).strip()
                rest = re.sub(r"^<[^>]*>", "", rest).strip()
            if cmd not in NAME_ARG_CMDS and cmd not in TEXT_ARG_CMDS:
                continue
            if cmd == "example" and rest:
                # 示例名写了但文件不存在 = 文档站里的死链（AGENTS.md 硬规则 11）：既接受仓库相对
                # 路径原样，也接受只写文件名（按示例目录 examples/demos/ 解析）。
                arg = rest.split()[0]
                hit = os.path.isfile(os.path.join(self.root, arg)) or os.path.isfile(
                    os.path.join(self.root, "examples", "demos", os.path.basename(arg)))
                if not hit:
                    self.add("DOC-R7", src.rel, note["line"], f"@example target file does not exist: {arg}")
                continue
            if rest:
                continue
            nxt = bodies[i + 1].strip() if i + 1 < len(bodies) else ""
            if nxt and not nxt.startswith("@") and not BLOCK_CMD_RE.match(nxt):
                continue  # 命令行的跨行续写：描述在紧随的一行里
            need = NAME_ARG_CMDS.get(cmd) or TEXT_ARG_CMDS.get(cmd)
            self.add("DOC-R7", src.rel, note["line"], f"@{cmd} missing {need}")

    # ---- DOC-R8 ----
    def check_body_notes(self, src: Source) -> None:
        """DOC-R8：///< 尾注只挂真实成员，且一个声明一条。

        体内代码不会进文档站，也不受 §13.5 的齐全度要求约束。给语句补 ///< 是「为了消门禁而写
        注释」的典型形态：既误导读者（以为这是可文档化的成员），又让真正的成员缺口被噪音盖住。
        函数 / 类型的尾注同样非法——Doxygen 会把它当成成员文档，但 §13.4 要求函数以 /// 块承载
        @param / @return，尾注形态必然缺矩阵项。
        """
        spans = src.body_spans()
        for note in src.masked:
            if note["kind"] != "doc" or not note["code_before"]:
                continue
            if not note["text"].startswith("///<"):
                continue
            if any(a <= note["line"] <= b for a, b in spans):
                self.add("DOC-R8", src.rel, note["line"],
                         "statements/local declarations inside a function body must not use ///< "
                         "trailing notes (not members, not documented; downgrade to // or delete)")
                continue
            code = src.code_lines[note["line"] - 1][:note["col"] - 1].strip()
            if not code:
                continue
            head = first_member(strip_attrs(code))
            if (looks_like_function(head) and not has_value_initializer(head)
                    and (FUNC_RE.match(strip_attrs(code)) or OPER_RE.match(strip_attrs(code)))):
                self.add("DOC-R8", src.rel, note["line"], "Function/operator must not use ///<; use a /// block above")
                continue
            if top_level_commas(head) >= 2:
                self.add("DOC-R8", src.rel, note["line"],
                         "multiple declarations on one line must not share a ///< (Doxygen attaches "
                         "it to only one); give each its own line")

    # ---- DOC-R4 / DOC-R5 ----
    def check_declarations(self, src: Source) -> None:
        for idx, decl, _end, access in src.declarations():
            line = idx + 1
            run = src.prev_comment_run(idx)
            names, void, ctor = parse_params(decl)
            head_member = first_member(strip_attrs(decl.strip()))
            # 类型 / 概念 / 别名没有形参：`concept X = requires(M m)` 的探测形参、`struct Y : Base<Z(0)>`
            # 之类括号、`using Fn = void (*)(int);` 的函数指针目标都不构成函数签名，按括号抓形参会凭空
            # 索要 @param。
            if TYPE_RE.match(head_member) or ALIAS_RE.match(head_member):
                names, void, ctor = [], False, False
            callable_decl = call_paren(head_member) >= 0 and bool(FUNC_RE.match(strip_attrs(head_member))
                                                                 or OPER_RE.match(strip_attrs(head_member)))
            is_func = callable_decl or (bool(names) and call_paren(head_member) >= 0)
            # 带值初始化器的声明是数据成员，不是函数：`std::function<void(int)> on_tick_ = [](int){};`
            # 的括号/花括号都在初始化表达式里，按函数形态判会凭空索要 @param/@return。
            if is_func and has_value_initializer(head_member):
                is_func, names, void, ctor = False, [], False, False
            # 命名空间包装（`namespace aurora {`）可写文档但非必写：矩阵里属「可选」，见 §13.5.2。
            optional = bool(NAMESPACE_RE.match(decl.strip()))
            # `= default` / `= delete` 的特殊成员按编译器语义平凡，§13.5.1 明确豁免；早先逐声明判定
            # 漏掉了这一条，于是 `X(const X&) = delete;` 也被要求补 @brief/@param。
            trivial = bool(re.search(r"=\s*(?:default|delete)\s*[;]?\s*$", first_member(decl).strip()))
            qualified = is_qualified_definition(head_member)
            doc_required = (access == "public" and not optional and not trivial and not qualified
                            and not FWD_DECL_RE.match(decl.strip()) and not FRIEND_RE.match(decl))
            if run is None:
                if not doc_required:
                    continue
                if self.trailing_doc(src, idx, _end):
                    continue        # 成员尾注 ///< 即其文档，不再要求上方注释块
                if TYPE_RE.match(decl):
                    self.add("DOC-R5", src.rel, line, "public type declaration missing doc comment")
                elif is_func:
                    self.add("DOC-R5", src.rel, line, "public function declaration missing doc comment")
                elif ALIAS_RE.match(decl):
                    self.add("DOC-R5", src.rel, line, "public type alias missing doc comment")
                elif VAR_RE.match(decl):
                    self.add("DOC-R5", src.rel, line, "public data member missing doc comment")
                continue
            kind, start, end, text = run
            if kind in ("plain", "block_plain"):
                if doc_required:
                    self.add("DOC-R4", src.rel, start, "// comment adjacent to a public declaration must become ///")
                continue
            if kind not in ("doc", "doc_bad", "blockdoc"):
                continue
            block = [n for n in src.masked if start <= n["line"] <= end and n["kind"] in ("doc", "doc_bad")]
            if src.rel.startswith("include/") and block:
                self.check_layout(src, block)
            # 「写则写全」（§13.5.2）：必写面仍限定 public（§13.5.1），但一旦写了 /// 块，完整性判据
            # 与公共成员同级——Doxygen 的 WARN_NO_PARAMDOC 对所有被抽取成员（含 protected）一视同仁，
            # 只写 @brief 不写 @param/@return 会在文档站里留下半截签名。
            enforce = doc_required or (access in ("public", "protected") and not trivial and not qualified)
            # 反向判据与「必写面」无关，只要写了 /// 块就查：Doxygen 拿块里写出的名字去对签名，
            # 对不上必告警。`= delete` / `= default` 的特殊成员同样被抽取，块里写 `@param Surface`
            # 一样报 "argument 'Surface' of command @param is not found in the argument list"——
            # §13.5.1 的豁免只免「必须写」，未免「写了要对着签名」。
            if is_func:
                docd = documented_params(text)
                extra = [d for d in docd if d not in names]
                if extra:
                    self.add("DOC-R7", src.rel, start,
                             "@param name not in the signature parameter list: " + ", ".join(extra))
                # 同一形参写两条 @param：Doxygen 报 "argument X ... has multiple @param documentation
                # sections" 与 "too many @param commands. Found N while function has M parameter"，
                # 文档站只呈现其中一条，另一条无声丢失；改名残留（旧名 + 新名各写一份）也落在这里。
                allp = documented_params(text, dedupe=False)
                dup = [d for d in dict.fromkeys(allp) if allp.count(d) > 1]
                if dup:
                    self.add("DOC-R7", src.rel, start,
                             "@param documents the same parameter more than once: " + ", ".join(dup))
            tparams = template_params(strip_attrs(decl))
            if tparams:
                doc_tp = documented_tparams(text)
                extra_tp = [d for d in doc_tp if d not in tparams]
                if extra_tp:
                    self.add("DOC-R7", src.rel, start,
                             "@tparam name not in the template parameter list: " + ", ".join(extra_tp))
                all_tp = documented_tparams(text, dedupe=False)
                dup_tp = [d for d in dict.fromkeys(all_tp) if all_tp.count(d) > 1]
                if dup_tp:
                    self.add("DOC-R7", src.rel, start,
                             "@tparam documents the same template parameter more than once: " + ", ".join(dup_tp))
            # 同一矩阵的「不适用」侧：void 函数与构造/析构不写 @return。写了不是冗余而是误导——
            # Doxygen 会把它渲染成 Returns 段（并报 "found documented return type ... that does not
            # return anything"），读者据此以为存在返回通道。返回类型只看挂注释的那一个成员，
            # 避免 fold 合并的相邻成员污染判定。
            _n0, m_void, m_ctor = parse_params(head_member)
            if is_func and (m_void or m_ctor) and "@return" in text:
                self.add("DOC-R7", src.rel, start,
                         "void / ctor-dtor must not document @return (Section 13.5.2 'Not applicable' column)")
            if not enforce:
                continue
            if "@brief" not in text:
                self.add("DOC-R5", src.rel, start, "doc comment missing @brief")
            missing = [p for p in names if not has_param(text, p)]
            if missing:
                self.add("DOC-R5", src.rel, start, "missing @param: " + ", ".join(missing))
            if is_func and not void and not ctor and "@return" not in text and "@tparam" not in text:
                self.add("DOC-R5", src.rel, start, "missing @return")

    @staticmethod
    def trailing_doc(src: Source, start_idx: int, end_idx: int) -> bool:
        """声明行（或其折叠尾行）自带 ///< 尾注时，视为已文档化。"""
        for ln in (start_idx + 1, end_idx + 1):
            note = src.note_at(ln)
            if note and note["code_before"] and note["kind"] in ("doc", "doc_bad"):
                return True
        return False

    # ---- 矩阵补充项：文件级块 / 模板形参 / 枚举项 / 常量宏 ----
    def check_matrix_extras(self, src: Source) -> None:
        self.check_file_level(src)
        for idx, decl, end, access in src.declarations():
            if access != "public":
                continue
            stripped = strip_attrs(decl)
            if is_qualified_definition(stripped):
                continue        # 类外限定定义：文档契约在类内声明处，不在此重复索要 @tparam
            run = src.prev_comment_run(idx)
            text = run[3] if run else ""
            # 每个具名模板形参须有一条 @tparam
            tparams = template_params(stripped)
            if tparams and run:
                missing = [t for t in tparams if not has_tparam(text, t)]
                if missing:
                    self.add("DOC-R5", src.rel, run[1], "missing @tparam: " + ", ".join(missing))
            # 枚举体内每个枚举项须有一行说明
            if ENUM_RE.match(stripped):
                self.check_enum_items(src, idx, end)
            # 常量宏（无参 #define）须有一行说明
            mm = MACRO_RE.match(stripped)
            if mm and "(" not in mm.group(1) and not mm.group(1).endswith("_H"):
                if run is None and not self.trailing_doc(src, idx, end):
                    self.add("DOC-R5", src.rel, idx + 1,
                             "public constant macro missing a one-line note "
                             "(///< trailing note or adjacent /// block)")

    def check_enum_items(self, src: Source, decl_idx: int, decl_end: int) -> None:
        """枚举体内每个具名枚举项须有 ///< 尾注或紧邻 /// 块。"""
        depth, j = 0, decl_idx
        body_lines: list[int] = []
        while j < len(src.lines) and j - decl_idx <= 400:
            code = re.sub(r"//.*$", "", src.masked_lines[j])
            before = depth
            depth += code.count("{") - code.count("}")
            if before > 0:
                body_lines.append(j)
            if before > 0 and depth == 0:
                break
            j += 1
        for ln0 in body_lines:
            code = re.sub(r"//.*$", "", src.masked_lines[ln0]).strip().rstrip(",")
            item = ENUM_ITEM_RE.match(code)
            if not item:
                continue
            if self.trailing_doc(src, ln0, ln0) or src.prev_comment_run(ln0) is not None:
                continue
            self.add("DOC-R5", src.rel, ln0 + 1, f"enum item {item.group(1)} missing a one-line note")

    def check_file_level(self, src: Source) -> None:
        """include/ 头文件须有文件级 /// 块且含 @brief。"""
        if not src.rel.endswith((".h", ".hpp", ".inl")):
            return
        head = "\n".join(src.lines[:80])
        if re.search(r"^///\s*@(?:brief|file)\b", head, re.M):
            return
        self.add("DOC-R5", src.rel, 1, "header missing the file-level /// block (must include @brief)")

    def run(self, roots: list[str]) -> None:
        for base in roots:
            top = os.path.join(self.root, base)
            if not os.path.isdir(top):
                continue
            for dirpath, dirnames, filenames in os.walk(top):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(SKIP_DIR_PREFIX)]
                for name in sorted(filenames):
                    if name.endswith(SOURCE_EXTS):
                        self.check(os.path.join(dirpath, name))


def report(findings: list[dict], limit: int) -> None:
    by_rule = Counter(f["rule"] for f in findings)
    by_dir: dict[str, Counter] = defaultdict(Counter)
    for f in findings:
        by_dir[f["file"].split("/")[0]][f["rule"]] += 1
    print("=== Doxygen doc-comment gate ===")
    for rule in sorted(by_rule):
        print(f"  {rule:8s} {by_rule[rule]:6d}")
    print("--- by directory ---")
    for d in sorted(by_dir):
        print(f"  {d:10s} " + "  ".join(f"{k}={v}" for k, v in sorted(by_dir[d].items())))
    print(f"Total {len(findings)} finding(s)")
    for f in findings[:limit]:
        print(f"  {f['file']}:{f['line']}: [{f['rule']}] {f['msg']}")


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description="Doxygen doc-comment compliance gate")
    ap.add_argument("--root", default=".")
    ap.add_argument("--roots", default=",".join(DEFAULT_ROOTS))
    ap.add_argument("--rules", default="", help="only check these rules (comma-separated DOC-R1..DOC-R8)")
    ap.add_argument("--files", default="", help="only report paths matching these prefixes (comma-separated)")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--report", default="")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.root):
        print(f"Error: root directory not found: {args.root}", file=sys.stderr)
        return 2
    wanted = {r.strip().upper() for r in args.rules.split(",") if r.strip()}
    prefixes = [p.strip().replace("\\", "/") for p in args.files.split(",") if p.strip()]

    checker = Checker(args.root)
    try:
        checker.run([r.strip() for r in args.roots.split(",") if r.strip()])
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2
    findings = checker.findings
    if wanted:
        findings = [f for f in findings if f["rule"] in wanted]
    if prefixes:
        findings = [f for f in findings if any(f["file"].startswith(p) for p in prefixes)]
    report(findings, args.limit)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            json.dump(findings, fh, ensure_ascii=False, indent=1)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
