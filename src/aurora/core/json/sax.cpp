// SAX 解析引擎（公共出口 parse_sax）。
//
// 本文件持有**唯一的字符级递归下降引擎**：DOM 出口（parse.cpp 的 DomBuilder）与 SAX 出口都经由它，
// 两套出口因此共享同一份词法、转义解码、数字分派与错误定位逻辑，行为不可能分叉。

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aurora/core/json.h"
#include "aurora/core/utf8.h"

namespace aurora::json {

namespace {

/// @brief 偏移量 → (行, 列)，均从 1 起（`\n` 计数换行）。
[[nodiscard]] auto line_col_of(std::string_view input, std::size_t offset) noexcept
    -> std::pair<std::size_t, std::size_t> {
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
        len = std::min<std::size_t>(len, 24);
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
    // 原实现先把数字字符收进 std::string 再去首尾零；libc++ 下 string 的构造
    // 可能抛（bad_alloc），会令本 noexcept 函数被 bugprone-exception-escape 点名
    // （wasm lint 实测）。改为直接定位数字序列的首 / 末非零位，行为等价、零分配：
    // 有效位数 = 末位非零下标 - 首位非零下标 + 1（全零 / 无数字为 0）。
    std::size_t first = mantissa_end;
    std::size_t last = mantissa_end;
    for (std::size_t i = 0; i < mantissa_end; ++i) {
        const char c = num[i];
        if (c >= '1' && c <= '9') {
            if (first == mantissa_end) {
                first = i;
            }
            last = i;
        }
    }
    if (first == mantissa_end) {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t i = first; i <= last; ++i) {
        const char c = num[i];
        if (c >= '0' && c <= '9') {
            ++count;
        }
    }
    return count;
}

/// @brief 字符级递归下降 SAX 引擎：只产出事件，不构建任何 DOM。
/// @note 内部方法的 `Result<bool>` 内层 bool 表示「是否继续」——`false` 即消费者要求提前终止。
class SaxCore {
  public:
    SaxCore(std::string_view input, const ParseOptions &opts, SaxHandler &handler) noexcept
        : in_(input), opts_(opts), handler_(handler) {}

    [[nodiscard]] auto run() -> Result<void> {
        if (in_.size() >= 3 && static_cast<unsigned char>(in_[0]) == 0xEFU &&
            static_cast<unsigned char>(in_[1]) == 0xBBU && static_cast<unsigned char>(in_[2]) == 0xBFU) {
            return parse_error_at(in_, 0, "leading UTF-8 BOM is not allowed");
        }
        skip_ws();
        if (at_end()) {
            return parse_error_at(in_, pos_, "empty input");
        }
        auto emitted = emit_value(0);
        if (!emitted.ok()) {
            return emitted.error();
        }
        if (!emitted.value()) {
            return {};  // 消费者主动终止：按成功返回
        }
        skip_ws();
        if (!at_end()) {
            return parse_error_at(in_, pos_, "trailing content after top-level value");
        }
        return {};
    }

  private:
    std::string_view in_;
    ParseOptions opts_;
    SaxHandler &handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) 解析器持处理器引用
    std::size_t pos_ = 0;
    std::string scratch_;  ///< 字符串解码 / 对象键的复用缓冲（回调期间有效）

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

    [[nodiscard]] auto match(std::string_view literal) noexcept -> bool {  // NOLINT(bugprone-exception-escape)
        if (in_.size() - pos_ < literal.size()) {
            return false;
        }
        // 首个分支已保证 pos_ + literal.size() <= size_，compare 的抛出条件
        // （pos > size）不可达；检查器无法建模该区间推理。
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

    [[nodiscard]] auto emit_value(std::size_t depth) -> Result<bool> {
        if (depth > opts_.max_depth) {
            return depth_error();
        }
        if (at_end()) {
            return parse_error_at(in_, pos_, "unexpected end of input");
        }
        const char c = peek();
        switch (c) {
            case '{':
                return emit_object(depth);
            case '[':
                return emit_array(depth);
            case '"': {
                auto text = scan_string();
                if (!text.ok()) {
                    return text.error();
                }
                return handler_.on_string(text.value());
            }
            case 't':
            case 'f':
            case 'n':
                return emit_keyword();
            default:
                break;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return emit_number();
        }
        return parse_error_at(in_, pos_, std::string("unexpected character '") + c + "'");
    }

    [[nodiscard]] auto emit_keyword() -> Result<bool> {
        if (match("true")) {
            return handler_.on_bool(true);
        }
        if (match("false")) {
            return handler_.on_bool(false);
        }
        if (match("null")) {
            return handler_.on_null();
        }
        return parse_error_at(in_, pos_, "invalid literal (expected true / false / null)");
    }

    [[nodiscard]] auto emit_object(std::size_t depth) -> Result<bool> {
        ++pos_;  // consume '{'
        if (!handler_.on_object_start()) {
            return false;
        }
        std::size_t count = 0;
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return handler_.on_object_end(0);
        }
        while (true) {
            skip_ws();
            if (at_end() || peek() != '"') {
                return parse_error_at(in_, pos_, "expected a string key");
            }
            auto key = scan_string();
            if (!key.ok()) {
                return key.error();
            }
            if (!handler_.on_object_key(key.value())) {
                return false;
            }
            skip_ws();
            if (at_end() || peek() != ':') {
                return parse_error_at(in_, pos_, "expected ':' after object key");
            }
            ++pos_;
            skip_ws();
            auto value = emit_value(depth + 1);
            if (!value.ok()) {
                return value.error();
            }
            if (!value.value()) {
                return false;
            }
            ++count;
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
        return handler_.on_object_end(count);
    }

    [[nodiscard]] auto emit_array(std::size_t depth) -> Result<bool> {
        ++pos_;  // consume '['
        if (!handler_.on_array_start()) {
            return false;
        }
        std::size_t count = 0;
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return handler_.on_array_end(0);
        }
        while (true) {
            skip_ws();
            auto value = emit_value(depth + 1);
            if (!value.ok()) {
                return value.error();
            }
            if (!value.value()) {
                return false;
            }
            ++count;
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
        return handler_.on_array_end(count);
    }

    /// @brief 扫描字符串字面量，转义解码进 `scratch_`，返回指向它的视图（回调期间有效）。
    [[nodiscard]] auto scan_string() -> Result<std::string_view> {
        ++pos_;  // consume opening quote
        scratch_.clear();
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
                auto esc = parse_escape(e);
                if (!esc.ok()) {
                    return esc.error();
                }
                continue;
            }
            if (c < 0x80U) {
                scratch_.push_back(static_cast<char>(c));
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
            scratch_.append(in_.substr(pos_, len));
            pos_ += len;
        }
        return std::string_view(scratch_);
    }

    [[nodiscard]] auto parse_escape(char e) -> Result<void> {
        switch (e) {
            case '"':
                scratch_.push_back('"');
                return {};
            case '\\':
                scratch_.push_back('\\');
                return {};
            case '/':
                scratch_.push_back('/');
                return {};
            case 'b':
                scratch_.push_back('\b');
                return {};
            case 'f':
                scratch_.push_back('\f');
                return {};
            case 'n':
                scratch_.push_back('\n');
                return {};
            case 'r':
                scratch_.push_back('\r');
                return {};
            case 't':
                scratch_.push_back('\t');
                return {};
            case 'u':
                return parse_unicode_escape();
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

    [[nodiscard]] auto parse_unicode_escape() -> Result<void> {
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
        scratch_ += utf8_encode(cp);
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

    [[nodiscard]] auto emit_number() -> Result<bool> {
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
        return dispatch_number(in_.substr(start, pos_ - start), is_float);
    }

    /// @brief 数字分派：整数字面量先 int64 再 uint64，溢出保真；小数字面量经 from_chars，
    ///        往返失真或域外一律落 RawNumber。
    [[nodiscard]] auto dispatch_number(std::string_view token, bool is_float) -> Result<bool> {
        const char *first = token.data();
        const char *last = std::to_address(token.end());
        if (!is_float) {
            if (token == "-0") {
                return handler_.on_int(0);  // JSON 无负零整数语义
            }
            std::int64_t i = 0;
            if (auto [ptr, ec] = std::from_chars(first, last, i); ec == std::errc{} && ptr == last) {
                return handler_.on_int(i);
            }
            std::uint64_t u = 0;
            if (auto [ptr, ec] = std::from_chars(first, last, u); ec == std::errc{} && ptr == last) {
                return handler_.on_uint(u);
            }
            return handler_.on_raw_number(token);
        }
        double d = 0.0;
        auto [ptr, ec] = std::from_chars(first, last, d);
        if (ec != std::errc{} || ptr != last) {
            return handler_.on_raw_number(token);  // 上溢 / 下溢 / 非法 → 保真
        }
        char buf[64];
        const auto [tp, tec] = std::to_chars(buf, std::end(buf), d);
        if (tec == std::errc{}) {
            const std::string_view shortest(buf, static_cast<std::size_t>(tp - buf));
            if (significant_digits(token) <= significant_digits(shortest)) {
                return handler_.on_double(d);
            }
        }
        return handler_.on_raw_number(token);
    }
};

}  // namespace

auto parse_sax(std::string_view input, SaxHandler &handler, ParseOptions opts) -> Result<void> {
    SaxCore core(input, opts, handler);
    return core.run();
}

}  // namespace aurora::json
