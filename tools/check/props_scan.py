#!/usr/bin/env python3
# ============================================================================
# props_scan.py - 属性面文本扫描的共用件（三门禁同源）
# ----------------------------------------------------------------------------
# 为什么单独一份：`check_props_table_coverage` / `check_widget_bind_pattern` /
# `check_no_optional_in_public_signature` 判的是同一张权威表的三个侧面（表内自洽、
# 控件接线、签名不外泄可空态）。它们必须对「什么算一个属性行」「公共面到哪为止」给出
# **同一个答案**——两份独立实现的解析器迟早给出两个答案，而两个答案都绿不代表口径成立。
#
# 本件只做解析与定位，不做判定、不打印、不返回退出码。
#
# 用法：三个门禁脚本与本文件同目录，直接 `import props_scan`（ctest 的 WORKING_DIRECTORY
# 为仓库根，故以 `__file__` 同目录解析导入路径）。
# ============================================================================
import os
import re

# 机制基类自身不是属性面，不计入「待迁移的 XxxProps」分母。
PROPS_DECL_RE = re.compile(r"^\s*(?:class|struct)\s+(?!AbstractProps\b)(\w*Props)\b", re.M)
# 已迁移的判据：继承 `AbstractProps<自身>`（CRTP 实参即自身名，形态由机制层固定）。
MIGRATED_DECL_RE = re.compile(r"(?:class|struct)\s+(\w*Props)\s*(?:final\s*)?:\s*public\s+AbstractProps\s*<\s*\1\s*>")
# 控件侧：公有继承一枚 Props 的类（基类清单在 `:` 之后、`{` 之前）。
WIDGET_INHERITS_PROPS_RE = re.compile(r"^\s*class\s+(\w+)\s*(?:final\s*)?:((?:[^;{}]|{[^}]*})+?)\{", re.M)


def repo_root_of(path):
    """由脚本自身路径上溯到含 `CMakeLists.txt` 的仓库根；供 `--root` 缺省值使用。"""
    current = os.path.dirname(os.path.abspath(path))
    while current and current != os.path.dirname(current):
        if os.path.isfile(os.path.join(current, "CMakeLists.txt")):
            return current
        current = os.path.dirname(current)
    return current


def strip_comments(text):
    """去注释保行结构：注释字符替换为空格，字符串与字符字面量原样保留（键名在其中）。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c in "\"'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def matching(text, open_idx, open_ch, close_ch):
    """返回 (内容, 闭括号后的下标)；open_idx 指向 open_ch。找不到闭括号返回 (None, open_idx)。"""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i], i + 1
        i += 1
    return None, i


def angle_stripped(sig):
    """去掉尖括号内内容，便于判断「参数表之外是否还有括号」。"""
    out, depth, i = [], 0, 0
    while i < len(sig):
        c = sig[i]
        if c == "<":
            depth += 1
        elif c == ">" and depth:
            depth -= 1
        elif depth == 0:
            out.append(c)
        i += 1
    return "".join(out)


def line_of(text, idx):
    return text.count("\n", 0, idx) + 1


class Member:
    """一枚成员函数（含声明式纯虚件；体为空串）。"""
    __slots__ = ("name", "params", "ret", "body", "is_static", "access", "line", "decl")

    def __init__(self, name, params, ret, body, is_static, access, line, decl):
        self.name = name
        self.params = params
        self.ret = ret
        self.body = body
        self.is_static = is_static
        self.access = access
        self.line = line
        self.decl = decl

    def has_ctx_param(self):
        return "BuildContext" in self.params

    def signature(self):
        return f"{self.params} -> {self.ret}"


def parse_access_spans(body_text):
    """返回 [(offset, access)]，按 `public:` / `private:` / `protected:` 出现次序。"""
    spans = [(0, "public")]
    for m in re.finditer(r"^\s*(public|private|protected)\s*:", body_text, re.M):
        spans.append((m.start(), m.group(1)))
    return spans


def access_for(spans, offset):
    cur = "public"
    for pos, kind in spans:
        if pos <= offset:
            cur = kind
        else:
            break
    return cur


def parse_members(body_text, line_offset=0):
    """扫类体内的函数成员：`[static] auto <name>(<params>) [const] [-> ret] { ... }`。

    本库属性面一律走尾置返回类型（`CODING_STANDARDS.md` 的函数声明形态），故以 `auto` 起头
    定位；`line_offset` 把类体内的相对行号换算成文件行号。
    """
    members = []
    spans = parse_access_spans(body_text)
    for m in re.finditer(r"\bauto\s+(\w+)\s*\(", body_text):
        name = m.group(1)
        open_paren = m.end() - 1
        params, after_paren = matching(body_text, open_paren, "(", ")")
        if params is None:
            continue
        rest = body_text[after_paren:]
        brace = rest.find("{")
        semi = rest.find(";")
        if brace < 0:
            continue
        tail = angle_stripped(rest[:brace])
        # 返回类型取**原文**切片：`angle_stripped` 会连尖括号一起抹掉，`std::optional<T>` 就退化成
        # `std::optional`，签名外泄类判据（O1..O4）必须看得见模板实参。
        ret_m = re.search(r"->\s*([^{};]+?)\s*$", rest[:brace].strip())
        ret = ret_m.group(1).strip() if ret_m else ""
        prefix = body_text[:m.start()]
        is_static = bool(re.search(r"\bstatic\b[^\n]*$", prefix))
        access = access_for(spans, m.start())
        if 0 <= semi < brace:
            fbody = ""  # 声明而非定义（纯虚 / 外联实现）：只登记名字与签名
        else:
            fbody, _ = matching(body_text, after_paren + brace, "{", "}")
            if fbody is None:
                continue
        decl = angle_stripped(body_text[m.start():after_paren]) + " " + tail.strip()
        members.append(Member(name, params.strip(), ret, fbody, is_static, access,
                              line_of(body_text, m.start()) + line_offset, decl.strip()))
    return members


def parse_fields(body_text, spans):
    """私有数据成员：以 `name_;` 或 `name_ = ...;` 收尾、且落在 private 区的行（本库一行一枚）。"""
    fields = []
    lines = body_text.splitlines()
    prefix_len = 0
    for idx, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.endswith(";") and not stripped.startswith("friend"):
            head = angle_stripped(stripped.split("=")[0])
            if "(" not in head:
                fm = re.search(r"\b([a-z][a-z0-9_]*)_\s*(?:=\s*.*)?;$", stripped)
                if fm and access_for(spans, prefix_len) == "private":
                    fields.append((fm.group(1) + "_", idx))
        prefix_len += len(line) + 1
    return fields


def class_body(text, decl_match):
    """由类声明匹配取 (类体文本, 类体起始下标, 类体起始行号)；解析失败返回 (None, 0, 0)。

    匹配串可能已经吃掉开括号（继承清单型正则以 `{` 收尾），也可能停在开括号之前（CRTP 声明型
    正则停在 `>`）—— 两种都接受，从停点起找**第一枚**开括号。
    """
    stop = decl_match.end()
    if text[stop - 1] == "{":
        brace = stop - 1
    else:
        brace = text.find("{", stop)
    if brace < 0:
        return None, 0, 0
    body, _ = matching(text, brace, "{", "}")
    if body is None:
        return None, 0, 0
    return body, brace, line_of(text, brace) - 1


class Ctor:
    """一枚构造函数：`init` 为成员/基类初始化清单原文（无则空串），`body` 为函数体。

    `declared_only` 为真表示头里没有体（纯声明，体在 `.cpp`，或 `= default` / `= delete`），
    此时 `defaulted` 进一步区分形态：`""` 待类外定义、`"default"` 有默认实现、`"delete"` 已删除。
    """
    __slots__ = ("params", "init", "body", "access", "line", "declared_only", "defaulted")

    def __init__(self, params, init, body, access, line, declared_only, defaulted):
        self.params = params
        self.init = init
        self.body = body
        self.access = access
        self.line = line
        self.declared_only = declared_only
        self.defaulted = defaulted

    def delegates(self, class_name):
        """转调同类的另一个构造（`: Button(...)`）—— 绑定由被转调者兑现，本件不必再 `bind`。"""
        return bool(re.search(r"\b" + re.escape(class_name) + r"\s*\(", self.init))


def parse_ctors(body_text, class_name, line_offset=0):
    """扫类体内的构造函数：`[explicit] ClassName(<params>) [: init-list] { body }`、`;`、`= default;`。

    本库控件构造一律写在类体内（`CODING_STANDARDS.md` 的类内声明形态），故以行首锚定；
    纯声明与 `= default` / `= delete` 归入 `declared_only`，由调用方决定是补查类外定义还是判红。
    """
    ctors = []
    spans = parse_access_spans(body_text)
    pattern = re.compile(r"^[ \t]*(?:(?:explicit|constexpr)\s+)*" + re.escape(class_name) + r"\s*\(", re.M)
    for m in pattern.finditer(body_text):
        params, after = matching(body_text, m.end() - 1, "(", ")")
        if params is None:
            continue
        rest = body_text[after:]
        brace = rest.find("{")
        semi = rest.find(";")
        ends_before_body = 0 <= semi < brace if brace >= 0 else semi >= 0
        access = access_for(spans, m.start())
        line = line_of(body_text, m.start()) + line_offset
        if ends_before_body:
            head = rest[:semi]
            form = "delete" if "= delete" in head else ("default" if "= default" in head else "")
            ctors.append(Ctor(params.strip(), "", "", access, line, True, form))
            continue
        if brace < 0:
            continue
        init = angle_stripped(rest[:brace]).strip()
        cbody, _end = matching(body_text, after + brace, "{", "}")
        if cbody is None:
            continue
        ctors.append(Ctor(params.strip(), init, cbody, access, line, False, False))
    return ctors


def out_of_line_ctors(text, class_name):
    """`.cpp` 里的类外构造定义：`ClassName::ClassName(<params>) [: init-list] { body }`。

    返回 [(参数表, 初始化清单, 函数体, 行号)]，供「头里只有声明」的构造补全判定面。
    """
    found = []
    pattern = re.compile(r"^[ \t]*" + re.escape(class_name) + r"::" + re.escape(class_name) + r"\s*\(", re.M)
    for m in pattern.finditer(text):
        params, after = matching(text, m.end() - 1, "(", ")")
        if params is None:
            continue
        rest = text[after:]
        brace = rest.find("{")
        semi = rest.find(";")
        if brace < 0 or (0 <= semi < brace):
            continue
        init = angle_stripped(rest[:brace]).strip()
        body, _end = matching(text, after + brace, "{", "}")
        if body is None:
            continue
        found.append((params.strip(), init, body, line_of(text, m.start())))
    return found


def migrated_props_classes(text):
    """返回 [(类名, 类体, 起始行偏移)] —— 已迁移的属性面。"""
    found = []
    for m in MIGRATED_DECL_RE.finditer(text):
        body, _brace, line_offset = class_body(text, m)
        if body is not None:
            found.append((m.group(1), body, line_offset))
    return found


def base_list(clause):
    """`public A, public B` 形式的基类清单，返回 [(access, name)]，保持书写次序。"""
    out = []
    for part in clause.split(","):
        part = part.strip()
        if not part:
            continue
        am = re.match(r"^(virtual\s+)?(public|protected|private)?\s*(.+)$", part)
        access = am.group(2) or "public"
        name = re.sub(r"<.*>", "", am.group(3).strip(), flags=re.S)
        out.append((access, name))
    return out


def header_files(root):
    """受检公共头清单：include/ 下的 .h/.hpp/.inl，路径以仓库根相对形式返回。"""
    base = os.path.join(root, "include")
    if not os.path.isdir(base):
        return
    for dirpath, _dirnames, filenames in os.walk(base):
        for fn in sorted(filenames):
            if not fn.endswith((".h", ".hpp", ".inl")):
                continue
            path = os.path.join(dirpath, fn)
            yield path, os.path.relpath(path, root).replace(os.sep, "/")


def read_stripped(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return strip_comments(handle.read())


def source_files(root):
    """`src/` 下的实现文件清单（类外构造定义的落点），产出与 `header_files` 同形的 (绝对路径, 仓库相对路径)。"""
    base = os.path.join(root, "src")
    if not os.path.isdir(base):
        return
    for dirpath, _dirnames, filenames in os.walk(base):
        for fn in sorted(filenames):
            if not fn.endswith((".cpp", ".cc", ".inl")):
                continue
            path = os.path.join(dirpath, fn)
            yield path, os.path.relpath(path, root).replace(os.sep, "/")
