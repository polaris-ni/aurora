#!/usr/bin/env python3
"""
gen_plural_rules.py - compile CLDR supplemental plural rules into C++ evaluation tables.

Inputs : CLDR supplemental JSON (data NOT vendored, same policy as the UCD width tables):
           - plurals.json  -> supplemental["plurals-type-cardinal"]  (locale -> pluralRule-count-<cat>)
           - ordinals.json -> supplemental["plurals-type-ordinal"]
Outputs (both carry an AUTO-GENERATED first line):
           - src/aurora/i18n/plural_rules.gen.cpp      checked-in derived table (one TU; private decl lives
                                                       in the src-side detail header)
           - tests/unit/plural_rules_samples.gen.inc   test fixture built from CLDR @integer/@decimal samples

Fetch (CLDR 48 / Unicode 16.0.0), from repo root:
  git clone --depth=1 --filter=blob:none --sparse https://github.com/unicode-org/cldr-json.git build-tmp-cldr/cldr-json
  cd build-tmp-cldr/cldr-json && git sparse-checkout set --cone cldr-json/cldr-core/supplemental
  # files at cldr-json/cldr-core/supplemental/{plurals,ordinals}.json

Rule grammar (CLDR TR35 plural rules, the subset the data uses):
  condition := and_cond ('or' and_cond)*
  and_cond  := relation ('and' relation)*
  relation  := expr ('=' | '!=' | 'within') rhs
  expr      := operand ('%' value)?
  operand   := 'n' | 'i' | 'v' | 'w' | 'f' | 't' | 'c' | 'e'   (c == e; compact exponent, unsupported -> 0)
  rhs       := item ((',' | 'or') item)*
  item      := value ('..' value)?
Everything from '@' on is sample data. An 'other' key carries no condition (implicit fallback).
Deterministic: language keys sorted; identical input -> byte-identical output.
"""
import argparse
import json
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except (AttributeError, OSError):
    pass

CATEGORIES = ("zero", "one", "two", "few", "many")  # 'other' is the implicit fallback
CATEGORY_CPP = {
    "zero": "aurora::PluralCategory::Zero",
    "one": "aurora::PluralCategory::One",
    "two": "aurora::PluralCategory::Two",
    "few": "aurora::PluralCategory::Few",
    "many": "aurora::PluralCategory::Many",
    "other": "aurora::PluralCategory::Other",
}
# operand -> (C++ rvalue, is_floating); 'c'/'e' (compact exponent) are unsupported and pinned to 0
OPERANDS = {
    "n": ("ops.n", True),
    "i": ("ops.i", False),
    "v": ("ops.v", False),
    "w": ("ops.w", False),
    "f": ("ops.f", False),
    "t": ("ops.t", False),
    "c": ("0LL", False),
    "e": ("0LL", False),
}


def tokenize(text):
    """Split a rule condition into (kind, value) tokens."""
    tokens, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch.isspace():
            i += 1
            continue
        if ch.isdigit() or (ch == "." and i + 1 < n and text[i + 1].isdigit() and not text.startswith("..", i)):
            j = i
            seen_dot = False
            while j < n:
                cj = text[j]
                if cj.isdigit():
                    j += 1
                    continue
                # '..' 是区间符号；只允许一段小数点，且其后必须紧跟数字
                if cj == "." and not seen_dot and j + 1 < n and text[j + 1].isdigit() and not text.startswith("..", j):
                    seen_dot = True
                    j += 1
                    continue
                break
            tokens.append(("num", text[i:j]))
            i = j
            continue
        if ch.isalpha():
            j = i
            while j < n and text[j].isalpha():
                j += 1
            word = text[i:j]
            tokens.append(("kw" if word in ("and", "or", "within") else "op", word))
            i = j
            continue
        if text.startswith("!=", i):
            tokens.append(("sym", "!="))
            i += 2
            continue
        if text.startswith("..", i):
            tokens.append(("sym", ".."))
            i += 2
            continue
        if ch in "=%,":
            tokens.append(("sym", ch))
            i += 1
            continue
        raise ValueError(f"unexpected character {ch!r} in rule: {text}")
    return tokens


class Parser:
    """Recursive-descent parser producing nested tuple AST: ('and'|'or'|'rel', ...)."""

    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else None

    def eat(self, kind=None, value=None):
        tok = self.peek()
        if tok is None or (kind is not None and tok[0] != kind) or (value is not None and tok[1] != value):
            raise ValueError(f"expected {kind}/{value}, got {tok}")
        self.pos += 1
        return tok

    def parse_condition(self):
        node = self.parse_and()
        while self.peek() == ("kw", "or"):
            self.eat()
            node = ("or", node, self.parse_and())
        return node

    def parse_and(self):
        node = self.parse_relation()
        while self.peek() == ("kw", "and"):
            self.eat()
            node = ("and", node, self.parse_relation())
        return node

    def parse_relation(self):
        lhs = self.parse_expr()
        tok = self.eat()
        if tok[0] != "sym" or tok[1] not in ("=", "!=", "within"):
            raise ValueError(f"expected relation operator, got {tok}")
        negated = tok[1] == "!="
        rhs = [self.parse_item()]
        while True:
            tok = self.peek()
            # TR35 消歧：rhs 内的 'or' 分隔符后面必是数字；后跟操作数（字母）则是条件级连接词。
            is_rhs_or = (tok == ("kw", "or") and self.pos + 1 < len(self.tokens)
                         and self.tokens[self.pos + 1][0] == "num")
            if tok != ("sym", ",") and not is_rhs_or:
                break
            self.eat()
            rhs.append(self.parse_item())
        return ("rel", lhs, rhs, negated)

    def parse_expr(self):
        operand = self.eat("op")[1]
        if operand not in OPERANDS:
            raise ValueError(f"unknown operand {operand}")
        modulo = None
        if self.peek() == ("sym", "%"):
            self.eat()
            modulo = self.eat("num")[1]
        return ("expr", operand, modulo)

    def parse_item(self):
        lo = self.eat("num")[1]
        hi = None
        if self.peek() == ("sym", ".."):
            self.eat()
            hi = self.eat("num")[1]
        return (lo, hi)


def cpp_number(text, is_double):
    """Emit a numeric literal; doubles always carry a decimal point so C++ types stay unambiguous."""
    if is_double:
        return text if "." in text else text + ".0"
    if "." in text:
        raise ValueError(f"fractional literal {text} in integer context")
    return text


def cpp_operand_expr(expr):
    """Compile `operand ('%' value)?` to a C++ rvalue plus its type tag."""
    _, operand, modulo = expr
    target, is_double = OPERANDS[operand]
    if modulo is None:
        return target, is_double
    if is_double:
        return f"std::fmod({target}, {cpp_number(modulo, True)})", True
    return f"({target} % {cpp_number(modulo, False)}LL)", False


def cpp_relation(rel):
    """Compile one relation; the numeric type follows the LHS operand (`n` is double, rest integer)."""
    _, expr, items, negated = rel
    lhs, is_double = cpp_operand_expr(expr)
    parts = []
    for lo, hi in items:
        lo_cpp = cpp_number(lo, is_double)
        if hi is None:
            parts.append(f"{lhs} == {lo_cpp}")
        else:
            hi_cpp = cpp_number(hi, is_double)
            parts.append(f"({lhs} >= {lo_cpp} && {lhs} <= {hi_cpp})")
    body = "(" + " || ".join(parts) + ")" if len(parts) > 1 else parts[0]
    return f"!({body})" if negated else body


def cpp_condition(ast):
    if ast[0] == "and":
        return f"({cpp_condition(ast[1])} && {cpp_condition(ast[2])})"
    if ast[0] == "or":
        return f"({cpp_condition(ast[1])} || {cpp_condition(ast[2])})"
    return cpp_relation(ast)


def parse_rules(raw):
    """locale -> ordered [(category, condition_ast)]; 'other' and empty rules are implicit fallback."""
    rules = {}
    for locale, entries in raw.items():
        parsed = []
        for key, text in entries.items():
            category = key.rsplit("-", 1)[-1]
            if category == "other":
                continue
            condition = text.split("@", 1)[0].strip()
            if not condition:
                continue
            parsed.append((category, Parser(tokenize(condition)).parse_condition()))
        rules[locale] = parsed
    return rules


def extract_samples(text, limit=6):
    """Pull reproducible numbers from the '@integer ... @decimal ...' sample lists (compact '1c6' skipped)."""
    samples = []
    for section in text.split("@")[1:]:
        parts = section.split(None, 1)
        if not parts or parts[0] not in ("integer", "decimal"):
            continue
        for token in parts[1].split(","):
            token = token.strip().rstrip("…").strip()
            if not token or "c" in token or "…" in token:
                continue
            if "~" in token:
                lo, _, hi = token.partition("~")
                candidates = [lo]
                try:
                    if float(hi) - float(lo) <= 8:
                        candidates.append(hi)
                except ValueError:
                    pass
            else:
                candidates = [token]
            for value in candidates:
                if value not in samples:
                    samples.append(value)
            if len(samples) >= limit:
                return samples[:limit]
    return samples[:limit]


def emit_switch(kind, rules, languages, out):
    out.append(f"auto plural_rules_eval_{kind}(int index, const PluralOperands &ops) -> PluralCategory {{")
    out.append("    switch (index) {")
    for position, language in enumerate(languages):
        locale_rules = rules.get(language, [])
        out.append(f"    case {position}:  // {language}")
        if not locale_rules:
            out.append("        return aurora::PluralCategory::Other;")
        else:
            for category, ast in locale_rules:
                out.append(f"        if ({cpp_condition(ast)}) {{")
                out.append(f"            return {CATEGORY_CPP[category]};")
                out.append("        }")
            out.append("        return aurora::PluralCategory::Other;")
    out.append("    default:")
    out.append("        return aurora::PluralCategory::Other;")
    out.append("    }")
    out.append("}")


def emit_cpp(languages, cardinal, ordinal, cldr_version, unicode_version):
    out = [
        "// AUTO-GENERATED from CLDR plural rules — DO NOT EDIT.",
        f"// Source: cldr-json cldr-core/supplemental/{{plurals,ordinals}}.json (CLDR {cldr_version},",
        f"// Unicode {unicode_version}; data NOT vendored, see THIRD_PARTY_LICENSES.md §10).",
        "// Regenerate: python tools/gen/gen_plural_rules.py <plurals.json> <ordinals.json>",
        '#include "aurora/i18n/detail/plural_rules.h"',
        "",
        "#include <string_view>",
        "",
        "namespace aurora::detail {",
        "namespace {",
        f"constexpr std::string_view AURORA_RULE_LANGUAGES[{len(languages)}] = {{{', '.join(f'\"{k}\"' for k in languages)}}};",
    ]

    def masks(rules):
        values = []
        for language in languages:
            bits = 0
            for category, _ast in rules.get(language, []):
                bits |= 1 << CATEGORIES.index(category)
            values.append(str(bits))
        return values

    out.append(f"constexpr std::uint32_t AURORA_CARDINAL_MASKS[{len(languages)}] = {{{', '.join(masks(cardinal))}}};")
    out.append(f"constexpr std::uint32_t AURORA_ORDINAL_MASKS[{len(languages)}] = {{{', '.join(masks(ordinal))}}};")
    out.append("}  // namespace")
    out.append("")
    out.append("auto plural_rules_language_index(std::string_view key) -> int {")
    out.append("    std::size_t lo = 0;")
    out.append(f"    std::size_t hi = {len(languages)};")
    out.append("    while (lo < hi) {")
    out.append("        const std::size_t mid = lo + (hi - lo) / 2;")
    out.append("        const int cmp = key.compare(AURORA_RULE_LANGUAGES[mid]);")
    out.append("        if (cmp == 0) {")
    out.append("            return static_cast<int>(mid);")
    out.append("        }")
    out.append("        if (cmp < 0) {")
    out.append("            hi = mid;")
    out.append("        } else {")
    out.append("            lo = mid + 1;")
    out.append("        }")
    out.append("    }")
    out.append("    return -1;")
    out.append("}")
    out.append("")
    out.append("auto plural_rules_category_mask(int index, bool ordinal) -> std::uint32_t {")
    out.append("    if (index < 0) {")
    out.append("        return 0U;")
    out.append("    }")
    out.append("    const auto idx = static_cast<std::size_t>(index);")
    out.append("    return ordinal ? AURORA_ORDINAL_MASKS[idx] : AURORA_CARDINAL_MASKS[idx];")
    out.append("}")
    emit_switch("cardinal", cardinal, languages, out)
    out.append("")
    emit_switch("ordinal", ordinal, languages, out)
    out.append("")
    out.append("}  // namespace aurora::detail")
    out.append("")
    return "\n".join(out)


def emit_samples(cardinal_raw, ordinal_raw, cldr_version):
    lines = [
        "// AUTO-GENERATED from CLDR plural rules — test fixture — DO NOT EDIT.",
        f"// Source: cldr-json cldr-core/supplemental/{{plurals,ordinals}}.json (CLDR {cldr_version}).",
        "// Regenerate: python tools/gen/gen_plural_rules.py <plurals.json> <ordinals.json>",
        "#include <string_view>",
        "",
        '#include "aurora/i18n/plural.h"',
        "",
        # 两条就地豁免，理由写在生成器里而不是逐行标注（本文件每次重新生成都会覆盖人工编辑）：
        # - readability-identifier-naming：夹具整族沿用 `AURORA_` 前缀的宏式命名，与消费它的单测同名同形，
        #   改成 PascalCase 只会让「生成夹具 / 手写断言」两套写法并存。
        # - modernize-use-designated-initializers：逐行补 4 个字段名会让 3.5k 行数据表膨胀约 60%，
        #   而字段顺序由本生成器单点控制（下一行的构造顺序与 struct 声明同源），不存在错位的空间。
        # 生成的注释文本按硬规则走纯 ASCII（字面量不入中文），完整中文理由只留在本函数注释里。
        "// NOLINTBEGIN(readability-identifier-naming, modernize-use-designated-initializers):",
        "// generated CLDR sample table; the row type keeps this fixture's AURORA_ naming family and",
        "// the column order is fixed by the generator, so both checks are structural rather than bugs.",
        "struct AURORA_PLURAL_RULE_SAMPLE {",
        "    std::string_view language;",
        "    std::string_view number;  // decimal text, fed through plural_operands_from_decimal",
        "    aurora::PluralCategory expected;",
        "    bool ordinal;",
        "};",
        "",
        "inline constexpr AURORA_PLURAL_RULE_SAMPLE AURORA_PLURAL_RULE_SAMPLES[] = {",
    ]
    count = 0
    for is_ordinal, raw in ((False, cardinal_raw), (True, ordinal_raw)):
        for locale in sorted(raw):
            for key, text in raw[locale].items():
                category = key.rsplit("-", 1)[-1]
                for number in extract_samples(text):
                    lines.append(f"    {{{{\"{locale}\"}}, {{\"{number}\"}}, "
                                 f"{CATEGORY_CPP[category]}, {str(is_ordinal).lower()}}},")
                    count += 1
    lines.append("};")
    lines.append("// NOLINTEND(readability-identifier-naming, modernize-use-designated-initializers)")
    lines.append("")
    return "\n".join(lines), count


def main() -> int:
    ap = argparse.ArgumentParser(description="Compile CLDR plural rules into C++ tables.")
    ap.add_argument("plurals", help="path to CLDR plurals.json (cardinal)")
    ap.add_argument("ordinals", help="path to CLDR ordinals.json (ordinal)")
    ap.add_argument("--out-src", default="src/aurora/i18n/plural_rules.gen.cpp")
    ap.add_argument("--out-samples", default="tests/unit/plural_rules_samples.gen.inc")
    args = ap.parse_args()

    with open(args.plurals, encoding="utf-8") as handle:
        cardinal_doc = json.load(handle)
    with open(args.ordinals, encoding="utf-8") as handle:
        ordinal_doc = json.load(handle)
    version = cardinal_doc["supplemental"].get("version", {})
    cldr_version = version.get("_cldrVersion", "unknown")
    unicode_version = version.get("_unicodeVersion", "unknown")

    cardinal_raw = cardinal_doc["supplemental"]["plurals-type-cardinal"]
    ordinal_raw = ordinal_doc["supplemental"]["plurals-type-ordinal"]
    cardinal = parse_rules(cardinal_raw)
    ordinal = parse_rules(ordinal_raw)
    languages = sorted(set(cardinal) | set(ordinal))

    cpp_text = emit_cpp(languages, cardinal, ordinal, cldr_version, unicode_version)
    inc_text, sample_count = emit_samples(cardinal_raw, ordinal_raw, cldr_version)
    with open(args.out_src, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(cpp_text)
    with open(args.out_samples, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(inc_text)
    print(f"[gen_plural_rules] languages={len(languages)} cardinal={len(cardinal)} "
          f"ordinal={len(ordinal)} samples={sample_count} -> {args.out_src}, {args.out_samples}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
