#include "aurora/core/json.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace aurora::json {

namespace {

/// @brief 追加换行 + 缩进（indent < 0 为紧凑模式，不换行）。
auto write_indent(std::string &out, int indent, std::size_t depth) -> void {
    if (indent < 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent) * depth, ' ');
}

/// @brief 追加 `\uXXXX`（小写十六进制，4 位）。
auto write_u_escape(std::string &out, std::uint32_t cp) -> void {
    static constexpr char k_hex[] = "0123456789abcdef";
    out += "\\u";
    out.push_back(k_hex[(cp >> 12U) & 0xFU]);
    out.push_back(k_hex[(cp >> 8U) & 0xFU]);
    out.push_back(k_hex[(cp >> 4U) & 0xFU]);
    out.push_back(k_hex[cp & 0xFU]);
}

/// @brief 解码 `s[i]` 起始的 UTF-8 序列；非法则按单字节返回（保底不崩）。
[[nodiscard]] auto decode_utf8(std::string_view s, std::size_t i) noexcept -> std::pair<std::uint32_t, std::size_t> {
    const auto c0 = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if ((c0 & 0xE0U) == 0xC0U) {
        len = 2;
        cp = c0 & 0x1FU;
    } else if ((c0 & 0xF0U) == 0xE0U) {
        len = 3;
        cp = c0 & 0x0FU;
    } else if ((c0 & 0xF8U) == 0xF0U) {
        len = 4;
        cp = c0 & 0x07U;
    } else {
        return {c0, 1};
    }
    if (i + len > s.size()) {
        return {c0, 1};
    }
    for (std::size_t k = 1; k < len; ++k) {
        const auto ck = static_cast<unsigned char>(s[i + k]);
        if ((ck & 0xC0U) != 0x80U) {
            return {c0, 1};
        }
        cp = (cp << 6U) | (ck & 0x3FU);
    }
    return {cp, len};
}

/// @brief 转义写入字符串字面量（含引号）。`/` 不转义（RFC 惯例）。
auto write_string(std::string_view s, std::string &out, bool ensure_ascii) -> void {
    out.push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                continue;
            case '\\':
                out += "\\\\";
                continue;
            case '\b':
                out += "\\b";
                continue;
            case '\f':
                out += "\\f";
                continue;
            case '\n':
                out += "\\n";
                continue;
            case '\r':
                out += "\\r";
                continue;
            case '\t':
                out += "\\t";
                continue;
            default:
                break;
        }
        if (c < 0x20U) {
            write_u_escape(out, c);
            continue;
        }
        if (c < 0x80U) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (!ensure_ascii) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        const auto [cp, len] = decode_utf8(s, i);
        if (cp > 0xFFFFU) {
            const std::uint32_t v = cp - 0x10000U;
            write_u_escape(out, 0xD800U + (v >> 10U));
            write_u_escape(out, 0xDC00U + (v & 0x3FFU));
        } else {
            write_u_escape(out, cp);
        }
        i += len - 1;
    }
    out.push_back('"');
}

/// @brief 写入 Double：域外（NaN/Inf）硬失败；shortest round-trip；整值补 `.0` 后缀保类型可辨。
[[nodiscard]] auto write_double(std::string &out, double d) -> Result<void> {
    if (std::isnan(d) || std::isinf(d)) {
        ErrorParams params;
        params["detail"] = std::isnan(d) ? "NaN" : "Infinity";
        return make_error(ErrorCode::JsonValueNotSerializable, params);
    }
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
    if (ec != std::errc{}) {
        ErrorParams params;
        params["detail"] = "unrepresentable double";
        return make_error(ErrorCode::JsonValueNotSerializable, params);
    }
    const std::string_view text(buf, static_cast<std::size_t>(ptr - buf));
    out.append(text);
    if (text.find_first_of(".eE") == std::string_view::npos) {
        out += ".0";
    }
    return {};
}

[[nodiscard]] auto dump_value(const Value &v, std::string &out, const DumpOptions &opts, std::size_t depth) -> Result<void> {
    switch (v.type()) {
        case Type::Null:
            out += "null";
            return {};
        case Type::Bool:
            out += v.as_or<bool>(false) ? "true" : "false";
            return {};
        case Type::Int: {
            char buf[32];
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v.as_or<std::int64_t>(0));
            (void)ec;
            out.append(buf, static_cast<std::size_t>(ptr - buf));
            return {};
        }
        case Type::UInt: {
            char buf[32];
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v.as_or<std::uint64_t>(0));
            (void)ec;
            out.append(buf, static_cast<std::size_t>(ptr - buf));
            return {};
        }
        case Type::Double:
            return write_double(out, v.as_or<double>(0.0));
        case Type::RawNumber:
            out.append(v.as_raw_number().value());  // 保真：原样输出存储字面量
            return {};
        case Type::String:
            write_string(v.as_string().value(), out, opts.ensure_ascii);
            return {};
        case Type::Array: {
            if (v.empty()) {
                out += "[]";
                return {};
            }
            out.push_back('[');
            bool first = true;
            for (const Value &elem : v) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                write_indent(out, opts.indent, depth + 1);
                if (auto r = dump_value(elem, out, opts, depth + 1); !r.ok()) {
                    return r;
                }
            }
            write_indent(out, opts.indent, depth);
            out.push_back(']');
            return {};
        }
        case Type::Object: {
            if (v.empty()) {
                out += "{}";
                return {};
            }
            out.push_back('{');
            bool first = true;
            for (const Entry &e : v.entries()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                write_indent(out, opts.indent, depth + 1);
                write_string(e.key, out, opts.ensure_ascii);
                out.push_back(':');
                if (opts.indent >= 0) {
                    out.push_back(' ');
                }
                if (auto r = dump_value(e.value, out, opts, depth + 1); !r.ok()) {
                    return r;
                }
            }
            write_indent(out, opts.indent, depth);
            out.push_back('}');
            return {};
        }
    }
    return {};
}

}  // namespace

auto dump_into(const Value &v, std::string &out, DumpOptions opts) -> Result<void> { return dump_value(v, out, opts, 0); }

auto dump(const Value &v, DumpOptions opts) -> Result<std::string> {
    std::string out;
    if (auto r = dump_value(v, out, opts, 0); !r.ok()) {
        return r.error();
    }
    return out;
}

}  // namespace aurora::json
