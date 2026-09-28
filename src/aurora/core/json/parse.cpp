#include "aurora/core/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aurora/core/utf8.h"

namespace aurora::json {

namespace {

/// @brief 偏移量 → (行, 列)，均从 1 起（`\n` 计数换行）。
[[nodiscard]] auto line_col_of(std::string_view input, std::size_t offset) noexcept -> std::pair<std::size_t, std::size_t> {
    std::size_t line = 1;
    std::size_t col = 1;
    const std::size_t n = offset < input.size() ? offset : input.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (input[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    return {line, col};
}

/// @brief 构造 json-parse-error：自定义 message（含行/列/偏移），hint 附 ≤24 字节上下文片段。
[[nodiscard]] auto parse_error_at(std::string_view input, std::size_t offset, std::string_view detail) -> Error {
    const auto [line, col] = line_col_of(input, offset);
    std::string msg = "JSON parse failed at line " + std::to_string(line) + ", column " + std::to_string(col) +
                      " (offset " + std::to_string(offset) + "): " + std::string(detail);
    std::string hint;
    if (offset < input.size()) {
        std::size_t len = input.size() - offset;
        if (len > 24) {
            len = 24;
        }
        hint = "Near: \"";
        for (std::size_t i = 0; i < len; ++i) {
            const char c = input[offset + i];
            hint.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
        }
        hint.push_back('"');
    }
    return make_error(ErrorCode::JsonParseError, msg, ErrorParams{}, std::move(hint));
}

/// @brief 有效数字个数（忽略符号 / 小数点 / 指数 / 前导零 / 末尾零）。
[[nodiscard]] auto significant_digits(std::string_view num) noexcept -> std::size_t {
    std::size_t mantissa_end = num.size();
    for (std::size_t i = 0; i < num.size(); ++i) {
        const char c = num[i];
        if (c == 'e' || c == 'E') {
            mantissa_end = i;
            break;
        }
    }
    std::string digits;
    digits.reserve(mantissa_end);
    for (std::size_t i = 0; i < mantissa_end; ++i) {
        const char c = num[i];
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        }
    }
    std::size_t begin = 0;
    while (begin < digits.size() && digits[begin] == '0') {
        ++begin;
    }
    std::size_t end = digits.size();
    while (end > begin && digits[end - 1] == '0') {
        --end;
    }
    return end - begin;
}

/// @brief 手写递归下降解析器：DOM 出口。SAX 出口沿用同一字符级引擎（见后续切片）。
class Parser {
  public:
    Parser(std::string_view input, const ParseOptions &opts) noexcept : in_(input), opts_(opts) {}

    [[nodiscard]] auto run() -> Result<Value> {
        if (in_.size() >= 3 && static_cast<unsigned char>(in_[0]) == 0xEFU &&
            static_cast<unsigned char>(in_[1]) == 0xBBU && static_cast<unsigned char>(in_[2]) == 0xBFU) {
            return parse_error_at(in_, 0, "leading UTF-8 BOM is not allowed");
        }
        skip_ws();
        if (at_end()) {
            return parse_error_at(in_, pos_, "empty input");
        }
        auto value = parse_value(0);
        if (!value.ok()) {
            return value;
        }
        skip_ws();
        if (!at_end()) {
            return parse_error_at(in_, pos_, "trailing content after top-level value");
        }
        return value;
    }

  private:
    std::string_view in_;
    ParseOptions opts_;
    std::size_t pos_ = 0;

    [[nodiscard]] auto at_end() const noexcept -> bool { return pos_ >= in_.size(); }
    [[nodiscard]] auto peek() const noexcept -> char { return in_[pos_]; }

    auto skip_ws() noexcept -> void {
        while (pos_ < in_.size()) {
            const char c = in_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] auto match(std::string_view literal) noexcept -> bool {
        if (in_.size() - pos_ < literal.size()) {
            return false;
        }
        if (in_.compare(pos_, literal.size(), literal) != 0) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    [[nodiscard]] auto depth_error() const -> Error {
        const auto [line, col] = line_col_of(in_, pos_);
        ErrorParams params;
        params["max"] = std::to_string(opts_.max_depth);
        params["line"] = std::to_string(line);
        params["column"] = std::to_string(col);
        return make_error(ErrorCode::JsonDepthExceeded, params);
    }

    [[nodiscard]] auto parse_value(std::size_t depth) -> Result<Value> {
        if (depth > opts_.max_depth) {
            return depth_error();
        }
        if (at_end()) {
            return parse_error_at(in_, pos_, "unexpected end of input");
        }
        const char c = peek();
        switch (c) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                auto s = parse_string();
                if (!s.ok()) {
                    return s.error();
                }
                return Value(std::move(s.value()));
            }
            case 't':
            case 'f':
            case 'n':
                return parse_keyword();
            default:
                break;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number();
        }
        return parse_error_at(in_, pos_, std::string("unexpected character '") + c + "'");
    }

    [[nodiscard]] auto parse_keyword() -> Result<Value> {
        if (match("true")) {
            return Value(true);
        }
        if (match("false")) {
            return Value(false);
        }
        if (match("null")) {
            return Value(nullptr);
        }
        return parse_error_at(in_, pos_, "invalid literal (expected true / false / null)");
    }

    [[nodiscard]] auto parse_object(std::size_t depth) -> Result<Value> {
        ++pos_;  // consume '{'
        Value obj = Value::object();
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return obj;
        }
        while (true) {
            skip_ws();
            if (at_end() || peek() != '"') {
                return parse_error_at(in_, pos_, "expected a string key");
            }
            auto key = parse_string();
            if (!key.ok()) {
                return key.error();
            }
            skip_ws();
            if (at_end() || peek() != ':') {
                return parse_error_at(in_, pos_, "expected ':' after object key");
            }
            ++pos_;
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value.ok()) {
                return value;
            }
            obj.set(key.value(), std::move(value.value()));  // 重复键后值覆盖前值，位置保持首次插入处
            skip_ws();
            if (at_end()) {
                return parse_error_at(in_, pos_, "unterminated object");
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            return parse_error_at(in_, pos_, "expected ',' or '}' in object");
        }
        return obj;
    }

    [[nodiscard]] auto parse_array(std::size_t depth) -> Result<Value> {
        ++pos_;  // consume '['
        Value arr = Value::array();
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return arr;
        }
        while (true) {
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value.ok()) {
                return value;
            }
            arr.push_back(std::move(value.value()));
            skip_ws();
            if (at_end()) {
                return parse_error_at(in_, pos_, "unterminated array");
            }
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            return parse_error_at(in_, pos_, "expected ',' or ']' in array");
        }
        return arr;
    }

    [[nodiscard]] auto parse_string() -> Result<std::string> {
        ++pos_;  // consume opening quote
        std::string out;
        while (true) {
            if (at_end()) {
                return parse_error_at(in_, pos_, "unterminated string");
            }
            const auto c = static_cast<unsigned char>(in_[pos_]);
            if (c == '"') {
                ++pos_;
                break;
            }
            if (c < 0x20U) {
                return parse_error_at(in_, pos_, "unescaped control character in string");
            }
            if (c == '\\') {
                ++pos_;
                if (at_end()) {
                    return parse_error_at(in_, pos_, "unterminated escape sequence");
                }
                const char e = in_[pos_++];
                auto esc = parse_escape(e, out);
                if (!esc.ok()) {
                    return esc.error();
                }
                continue;
            }
            if (c < 0x80U) {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            std::size_t len = 1;
            if (opts_.validate_utf8) {
                len = valid_utf8_len(pos_);
                if (len == 0) {
                    return parse_error_at(in_, pos_, "invalid UTF-8 sequence in string");
                }
            }
            out.append(in_.substr(pos_, len));
            pos_ += len;
        }
        return out;
    }

    [[nodiscard]] auto parse_escape(char e, std::string &out) -> Result<void> {
        switch (e) {
            case '"':
                out.push_back('"');
                return {};
            case '\\':
                out.push_back('\\');
                return {};
            case '/':
                out.push_back('/');
                return {};
            case 'b':
                out.push_back('\b');
                return {};
            case 'f':
                out.push_back('\f');
                return {};
            case 'n':
                out.push_back('\n');
                return {};
            case 'r':
                out.push_back('\r');
                return {};
            case 't':
                out.push_back('\t');
                return {};
            case 'u':
                return parse_unicode_escape(out);
            default:
                return parse_error_at(in_, pos_ - 1, "invalid escape sequence");
        }
    }

    [[nodiscard]] auto parse_hex4() -> Result<std::uint32_t> {
        if (pos_ + 4 > in_.size()) {
            return parse_error_at(in_, pos_, "incomplete \\u escape");
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = in_[pos_++];
            v <<= 4U;
            if (ch >= '0' && ch <= '9') {
                v |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                v |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                v |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else {
                return parse_error_at(in_, pos_ - 1, "invalid hex digit in \\u escape");
            }
        }
        return v;
    }

    [[nodiscard]] auto parse_unicode_escape(std::string &out) -> Result<void> {
        auto hi = parse_hex4();
        if (!hi.ok()) {
            return hi.error();
        }
        std::uint32_t cp = hi.value();
        if (cp >= 0xD800U && cp <= 0xDBFFU) {  // 高代理项：必须紧随低代理项
            if (pos_ + 1 >= in_.size() || in_[pos_] != '\\' || in_[pos_ + 1] != 'u') {
                return parse_error_at(in_, pos_, "unpaired high surrogate");
            }
            pos_ += 2;
            auto lo = parse_hex4();
            if (!lo.ok()) {
                return lo.error();
            }
            const std::uint32_t low = lo.value();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return parse_error_at(in_, pos_, "invalid low surrogate");
            }
            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
        } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {  // 孤立低代理项
            return parse_error_at(in_, pos_, "unpaired low surrogate");
        }
        out += utf8_encode(cp);
        return {};
    }

    /// @brief 校验 `in_[p]` 起始的 UTF-8 序列（含续字节 / 过长 / 代理项 / 上界），返回序列字节数；0 = 非法。
    [[nodiscard]] auto valid_utf8_len(std::size_t p) const noexcept -> std::size_t {
        const auto c0 = static_cast<unsigned char>(in_[p]);
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
            return 0;
        }
        if (p + len > in_.size()) {
            return 0;
        }
        for (std::size_t i = 1; i < len; ++i) {
            const auto ci = static_cast<unsigned char>(in_[p + i]);
            if ((ci & 0xC0U) != 0x80U) {
                return 0;
            }
            cp = (cp << 6U) | (ci & 0x3FU);
        }
        if ((len == 2 && cp < 0x80U) || (len == 3 && cp < 0x800U) || (len == 4 && cp < 0x10000U)) {
            return 0;  // 过长编码
        }
        if (cp >= 0xD800U && cp <= 0xDFFFU) {
            return 0;  // 代理项
        }
        if (cp > 0x10FFFFU) {
            return 0;
        }
        return len;
    }

    [[nodiscard]] auto parse_number() -> Result<Value> {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        if (at_end()) {
            return parse_error_at(in_, start, "invalid number");
        }
        if (peek() == '0') {
            ++pos_;
            if (!at_end() && peek() >= '0' && peek() <= '9') {
                return parse_error_at(in_, pos_, "leading zero in number");
            }
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        } else {
            return parse_error_at(in_, pos_, "invalid number");
        }
        bool is_float = false;
        if (!at_end() && peek() == '.') {
            is_float = true;
            ++pos_;
            if (at_end() || peek() < '0' || peek() > '9') {
                return parse_error_at(in_, pos_, "expected a digit after the decimal point");
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            is_float = true;
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (at_end() || peek() < '0' || peek() > '9') {
                return parse_error_at(in_, pos_, "expected a digit in exponent");
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
        }
        return make_number(in_.substr(start, pos_ - start), is_float);
    }

    /// @brief 数字分派：整数字面量先 int64 再 uint64，溢出保真；小数字面量经 from_chars，
    ///        往返失真或域外一律落 RawNumber。
    [[nodiscard]] static auto make_number(std::string_view token, bool is_float) -> Result<Value> {
        const char *first = token.data();
        const char *last = token.data() + token.size();
        if (!is_float) {
            if (token == "-0") {
                return Value(static_cast<std::int64_t>(0));  // JSON 无负零整数语义
            }
            std::int64_t i = 0;
            if (auto [ptr, ec] = std::from_chars(first, last, i); ec == std::errc{} && ptr == last) {
                return Value(i);
            }
            std::uint64_t u = 0;
            if (auto [ptr, ec] = std::from_chars(first, last, u); ec == std::errc{} && ptr == last) {
                return Value(u);
            }
            return Value::raw_number(token);
        }
        double d = 0.0;
        auto [ptr, ec] = std::from_chars(first, last, d);
        if (ec != std::errc{} || ptr != last) {
            return Value::raw_number(token);  // 上溢 / 下溢 / 非法 → 保真
        }
        char buf[64];
        const auto [tp, tec] = std::to_chars(buf, buf + sizeof(buf), d);
        if (tec == std::errc{}) {
            const std::string_view shortest(buf, static_cast<std::size_t>(tp - buf));
            if (significant_digits(token) <= significant_digits(shortest)) {
                return Value(d);
            }
        }
        return Value::raw_number(token);
    }
};

}  // namespace

auto parse(std::string_view input, ParseOptions opts) -> Result<Value> {
    Parser parser(input, opts);
    return parser.run();
}

}  // namespace aurora::json
