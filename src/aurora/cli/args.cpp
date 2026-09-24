// args.h 的实现：字面量转换、argv 扫描与强类型取值。
// 规格：codespec/specification/09-cli.md（语法 §4、字面量 §5、错误码 §6）。
// 纪律：零异常、零标准输出；一切失败经 Result<T> + cli-* 错误码上报。

#include "aurora/cli/args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/cli/command.h"
#include "aurora/cli/literals.h"

namespace aurora::cli {
namespace {

// ------------------------------------------------------------ 文本小工具

[[nodiscard]] auto to_lower(std::string_view text) -> std::string {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief `std::from_chars` 只接受指针区间，故本文件全部指针算术集中在这两个函数里。
template <typename T>
[[nodiscard]] auto from_whole(std::string_view text, T &destination) -> bool {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 需要 [first, last)
    const std::pair<const char *, const char *> span{text.data(), text.data() + text.size()};
    const auto result = std::from_chars(span.first, span.second, destination);
    return result.ec == std::errc{} && result.ptr == span.second;
}

/// @brief 整数解析：`from_chars` 不接受前导 '+'，此处手工剥掉后再解析。
[[nodiscard]] auto parse_integer(std::string_view token) -> std::optional<std::int64_t> {
    if (token.empty()) {
        return std::nullopt;
    }
    std::string text{token};
    if (text.front() == '+') {
        text.erase(0, 1);
    }
    std::int64_t out = 0;
    if (!from_whole<std::int64_t>(text, out)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] auto parse_decimal(std::string_view token) -> std::optional<double> {
    if (token.empty()) {
        return std::nullopt;
    }
    std::string text{token};
    if (text.front() == '+') {
        text.erase(0, 1);
    }
    double out = 0.0;
    if (!from_whole<double>(text, out)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] auto looks_like_number(std::string_view token) -> bool {
    return parse_integer(token).has_value() || parse_decimal(token).has_value();
}

/// @brief token 是否长得像选项（`-x` / `--xxx`）；单字符 `-` 与负数不算。
[[nodiscard]] auto looks_like_option(std::string_view token) -> bool {
    return token.size() > 1U && token.front() == '-' && !looks_like_number(token);
}

/// @brief 编辑距离（只为「did you mean」服务，超长输入直接判不相似）。
[[nodiscard]] auto edit_distance(std::string_view a, std::string_view b) -> int {
    constexpr std::size_t limit = 64U;
    if (a.empty() || b.empty() || a.size() > limit || b.size() > limit) {
        return static_cast<int>(limit);
    }
    std::vector<int> previous(b.size() + 1U);
    std::vector<int> current(b.size() + 1U);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        previous[j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
        }
        previous = current;
    }
    return previous[b.size()];
}

/// @brief 候选里最相近者（编辑距离 ≤ 2）；无则空串。
[[nodiscard]] auto closest_candidate(std::string_view needle, const std::vector<std::string> &haystack) -> std::string {
    std::string best;
    int best_distance = 3;
    for (const auto &candidate : haystack) {
        const int distance = edit_distance(needle, candidate);
        if (distance < best_distance) {
            best_distance = distance;
            best = candidate;
        }
    }
    return best;
}

[[nodiscard]] auto display_long(std::string_view long_name) -> std::string { return "--" + std::string{long_name}; }

[[nodiscard]] auto join_comma(const std::vector<std::string> &items) -> std::string {
    std::string out;
    for (const auto &item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item;
    }
    return out;
}

// ------------------------------------------------------------ 字面量

[[nodiscard]] auto parse_length_literal(std::string_view token) -> std::optional<Length> {
    const std::string text = to_lower(token);
    if (text == "fill" || text == "match_parent") {
        return Length::expand();
    }
    if (text == "auto" || text == "wrap" || text == "wrap_content") {
        return Length::wrap();
    }
    if (text.ends_with("%")) {
        const auto fraction = parse_decimal(text.substr(0, text.size() - 1));
        if (!fraction || *fraction < 0.0 || *fraction > 100.0) {
            return std::nullopt;
        }
        return Length{LengthKind::Fraction, static_cast<float>(*fraction / 100.0)};
    }
    std::string_view digits{text};
    if (digits.ends_with("px")) {
        digits = digits.substr(0, digits.size() - 2);
    }
    const auto pixels = parse_decimal(digits);
    if (!pixels || *pixels < 0.0 || !std::isfinite(*pixels)) {
        return std::nullopt;
    }
    return Length{LengthKind::Fixed, static_cast<float>(*pixels)};
}

[[nodiscard]] auto parse_hex_digit(char c) -> std::optional<int> {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower >= 'a' && lower <= 'f') {
        return 10 + (lower - 'a');
    }
    return std::nullopt;
}

[[nodiscard]] auto parse_hex_pair(std::string_view text, std::size_t offset, bool doubled) -> std::optional<int> {
    const auto high = parse_hex_digit(text[offset]);
    if (!high) {
        return std::nullopt;
    }
    if (!doubled) {
        return *high * 17;  // #rgb 展开：每位自乘（f → 255）
    }
    const auto low = parse_hex_digit(text[offset + 1]);
    if (!low) {
        return std::nullopt;
    }
    return (*high << 4) | *low;
}

[[nodiscard]] auto parse_rgb_channels(std::string_view inner, int expected) -> std::optional<std::vector<int>> {
    std::vector<int> channels;
    std::size_t start = 0;
    while (start <= inner.size()) {
        const auto comma = inner.find(',', start);
        const std::string_view piece =
            (comma == std::string_view::npos) ? inner.substr(start) : inner.substr(start, comma - start);
        const auto value = parse_integer(piece);
        if (!value || *value < 0 || *value > 255) {
            return std::nullopt;
        }
        channels.push_back(static_cast<int>(*value));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    if (channels.size() != static_cast<std::size_t>(expected)) {
        return std::nullopt;
    }
    return channels;
}

[[nodiscard]] auto parse_color_literal(std::string_view token) -> std::optional<Color> {
    if (token.empty()) {
        return std::nullopt;
    }
    const std::string text{token};
    if (text.front() == '#') {
        const std::string_view body = std::string_view{text}.substr(1);
        const bool doubled = (body.size() == 6U || body.size() == 8U);
        if (!doubled && body.size() != 3U && body.size() != 4U) {
            return std::nullopt;
        }
        const std::size_t step = doubled ? 2U : 1U;
        std::vector<int> components;
        for (std::size_t i = 0; i + step <= body.size(); i += step) {
            const auto component = parse_hex_pair(body, i, doubled);
            if (!component) {
                return std::nullopt;
            }
            components.push_back(*component);
        }
        if (components.size() == 3U) {
            return Color{static_cast<std::uint8_t>(components[0]), static_cast<std::uint8_t>(components[1]),
                         static_cast<std::uint8_t>(components[2])};
        }
        if (components.size() == 4U) {
            return Color{static_cast<std::uint8_t>(components[0]), static_cast<std::uint8_t>(components[1]),
                         static_cast<std::uint8_t>(components[2]), static_cast<std::uint8_t>(components[3])};
        }
        return std::nullopt;
    }
    const std::string lowered = to_lower(text);
    if (lowered.starts_with("rgb(") && lowered.ends_with(')')) {
        const auto channels = parse_rgb_channels(std::string_view{lowered}.substr(4, lowered.size() - 5), 3);
        if (!channels) {
            return std::nullopt;
        }
        return Color{static_cast<std::uint8_t>((*channels)[0]), static_cast<std::uint8_t>((*channels)[1]),
                     static_cast<std::uint8_t>((*channels)[2])};
    }
    if (lowered.starts_with("rgba(") && lowered.ends_with(')')) {
        const auto channels = parse_rgb_channels(std::string_view{lowered}.substr(5, lowered.size() - 6), 4);
        if (!channels) {
            return std::nullopt;
        }
        return Color{static_cast<std::uint8_t>((*channels)[0]), static_cast<std::uint8_t>((*channels)[1]),
                     static_cast<std::uint8_t>((*channels)[2]), static_cast<std::uint8_t>((*channels)[3])};
    }
    return std::nullopt;
}

/// @brief 日志级别词表。`log_level_label` 只有正向三字母标签（TRC/DBG/…），故这里同时
///        接受全称、三字母短标签与 warn/warning 两种写法，保证与 Logger 双向可对。
struct LevelWord {
    std::string_view word;
    LogLevel level;
};

constexpr std::array<LevelWord, 13> AURORA_LEVEL_WORDS{{
    {.word = "trace", .level = LogLevel::Trace},
    {.word = "trc", .level = LogLevel::Trace},
    {.word = "debug", .level = LogLevel::Debug},
    {.word = "dbg", .level = LogLevel::Debug},
    {.word = "info", .level = LogLevel::Info},
    {.word = "inf", .level = LogLevel::Info},
    {.word = "warn", .level = LogLevel::Warn},
    {.word = "warning", .level = LogLevel::Warn},
    {.word = "wrn", .level = LogLevel::Warn},
    {.word = "error", .level = LogLevel::Error},
    {.word = "err", .level = LogLevel::Error},
    {.word = "fatal", .level = LogLevel::Fatal},
    {.word = "ftl", .level = LogLevel::Fatal},
}};

[[nodiscard]] auto parse_log_level_literal(std::string_view token) -> std::optional<LogLevel> {
    const std::string text = to_lower(token);
    for (const auto &entry : AURORA_LEVEL_WORDS) {
        if (entry.word == text) {
            return entry.level;
        }
    }
    return std::nullopt;
}

/// @brief 时长字面量 → 毫秒（`500` / `250ms` / `5s` / `2m` / `1h` / `1d`）。
[[nodiscard]] auto parse_duration_literal(std::string_view token) -> std::optional<std::int64_t> {
    struct Unit {
        std::string_view suffix;
        double multiplier;
    };
    constexpr std::array<Unit, 5> units{{
        {.suffix = "ms", .multiplier = 1.0},
        {.suffix = "s", .multiplier = 1'000.0},
        {.suffix = "m", .multiplier = 60'000.0},
        {.suffix = "h", .multiplier = 3'600'000.0},
        {.suffix = "d", .multiplier = 86'400'000.0},
    }};
    for (const auto &unit : units) {
        if (token.size() > unit.suffix.size() && token.ends_with(unit.suffix)) {
            const auto number = parse_decimal(token.substr(0, token.size() - unit.suffix.size()));
            if (!number || !std::isfinite(*number)) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(*number * unit.multiplier);
        }
    }
    const auto bare = parse_decimal(token);
    if (!bare || !std::isfinite(*bare)) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*bare);  // 无后缀 = 毫秒
}

using ValueRaw = Value::Raw;

[[nodiscard]] auto make_value(ValueKind kind, ValueRaw raw, std::string_view literal) -> Value {
    return detail::LiteralFactory::make(kind, std::move(raw), std::string{literal});
}

}  // namespace

// ------------------------------------------------------------ detail（字面量单一出口）

namespace detail {

auto convert_literal(ValueKind kind, std::string_view token, std::string_view option_display) -> Result<Value> {
    const std::string literal{token};
    switch (kind) {
        case ValueKind::Bool: {
            const std::string text = to_lower(token);
            if (text == "true" || text == "1" || text == "yes") {
                return make_value(kind, ValueRaw{true}, literal);
            }
            if (text == "false" || text == "0" || text == "no") {
                return make_value(kind, ValueRaw{false}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::Int: {
            if (const auto number = parse_integer(token)) {
                return make_value(kind, ValueRaw{*number}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::Duration: {
            if (const auto ms = parse_duration_literal(token)) {
                return make_value(kind, ValueRaw{*ms}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::Double: {
            if (const auto number = parse_decimal(token)) {
                return make_value(kind, ValueRaw{*number}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::String:
        case ValueKind::Enum:
            return make_value(kind, ValueRaw{literal}, literal);
        case ValueKind::Length: {
            if (const auto length = parse_length_literal(token)) {
                return make_value(kind, ValueRaw{*length}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::Color: {
            if (const auto color = parse_color_literal(token)) {
                return make_value(kind, ValueRaw{*color}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
        case ValueKind::LogLevel: {
            if (const auto level = parse_log_level_literal(token)) {
                return make_value(kind, ValueRaw{*level}, literal);
            }
            return invalid_literal(option_display, kind, token);
        }
    }
    return invalid_literal(option_display, kind, token);
}

auto invalid_literal(std::string_view option_display, ValueKind kind, std::string_view token) -> Error {
    return make_error(ErrorCode::CliInvalidValue, ErrorParams{{"option", std::string{option_display}},
                                                              {"kind", std::string{to_string(kind)}},
                                                              {"value", std::string{token}}});
}

auto is_numeric_kind(ValueKind kind) noexcept -> bool {
    return kind == ValueKind::Int || kind == ValueKind::Double || kind == ValueKind::Duration;
}

}  // namespace detail

namespace {

/// @brief 数值跨类型读出：Int/Duration/Double 之间允许无损加宽，有损即 `cli-invalid-value`。
template <typename T>
[[nodiscard]] auto narrow_or_ok(const Value &value, ValueKind want, double source) -> Result<T> {
    const auto converted = static_cast<T>(source);
    if (std::isfinite(source) && static_cast<double>(converted) != source) {
        return detail::invalid_literal(value.raw_text(), want, value.raw_text());
    }
    return converted;
}

}  // namespace

// ------------------------------------------------------------ 扫描器
//
// `Parser` 定义在 `aurora::cli` 而非匿名空间：它是 `Arguments` 的 friend，而匿名空间会
// 给它一个内部链接的独立类型，friend 声明（指向 `aurora::cli::Parser`）便不再匹配。

/**
 * @brief 一次 `parse` 的驱动：沿命令链下钻，逐 token 归类，收尾物化默认值。
 *
 * 命令树语义（git 式）：选项属于「命中命令链的最深命令」，父级选项须写在子命令名之前，
 * 同名选项合并时由最深层的显式取值胜出（叶级默认值不覆盖父级显式取值）。
 * 见 specification/09-cli.md §4.5。
 */
class Parser {
  public:
    Parser(const CommandSpec &root, std::span<const std::string_view> tokens, std::string_view program_name)
        : root_(&root), tokens_(tokens) {
        program_ = program_name.empty() ? std::string{root.name} : std::string{program_name};
        if (program_.empty()) {
            program_ = "program";
        }
    }

    [[nodiscard]] auto run() -> Result<Invocation> {
        levels_.push_back(Level{.spec = root_});
        if (auto error = scan(); error) {
            return *error;
        }
        auto slots = finish();
        // --help / --version 优先于必填/互斥结论：用户要的是说明书，不是报错。
        if (!early_ && pending_) {
            return *pending_;
        }
        auto positionals = flatten_positionals();
        Invocation invocation;
        invocation.outcome = early_.value_or(ParseOutcome::Ok);
        invocation.display_text = std::move(early_text_);
        invocation.arguments =
            Arguments{std::move(slots), std::move(positionals), std::move(rest_), chain(), matched(), program_};
        return invocation;
    }

  private:
    /// @brief 命令链上的一层：声明 + 本层已消费的选项槽与位置参数。
    struct Level {
        const CommandSpec *spec = nullptr;
        std::vector<Arguments::Slot> slots;
        std::vector<Value> positionals;
        std::size_t positional_cursor = 0;
        bool after_double_dash = false;
    };

    [[nodiscard]] auto chain() const -> std::vector<std::string> {
        std::vector<std::string> names;
        names.push_back(program_);
        for (std::size_t i = 1; i < levels_.size(); ++i) {
            names.emplace_back(levels_[i].spec->name);
        }
        return names;
    }

    [[nodiscard]] auto matched() const -> const CommandSpec * { return levels_.back().spec; }

    [[nodiscard]] auto command_display() const -> std::string {
        std::string out = program_;
        for (std::size_t i = 1; i < levels_.size(); ++i) {
            out += ' ';
            out += levels_[i].spec->name;
        }
        return out;
    }

    [[nodiscard]] auto flatten_positionals() const -> std::vector<Value> {
        std::vector<Value> all;
        for (const auto &level : levels_) {
            all.insert(all.end(), level.positionals.begin(), level.positionals.end());
        }
        return all;
    }

    /// @brief 记下首个待报错误（必填缺失 / 缺子命令 / 互斥冲突），扫描照常收尾。
    auto pending(Error error) -> void {
        if (!pending_) {
            pending_ = std::move(error);
        }
    }

    [[nodiscard]] auto scan() -> std::optional<Error> {
        for (cursor_ = 0; cursor_ < tokens_.size() && !early_; ++cursor_) {
            const std::string_view token = tokens_[cursor_];
            if (levels_.back().after_double_dash) {
                if (auto error = take_positional(token, true); error) {
                    return error;
                }
                continue;
            }
            if (token == "--") {
                levels_.back().after_double_dash = true;
                continue;
            }
            if (token == "--help") {
                request_help();
                continue;
            }
            if (token == "--version" && !matched()->version.empty()) {
                request_version();
                continue;
            }
            if (looks_like_option(token)) {
                if (auto error = consume_option_token(token); error) {
                    return error;
                }
                continue;
            }
            if (auto error = consume_bare_token(token); error) {
                return error;
            }
        }
        return std::nullopt;
    }

    auto request_help() -> void {
        early_ = ParseOutcome::Help;
        early_text_ = help_text(*matched(), chain());
    }

    /// @brief 内建版本结局：仅在该层声明了 `version` 时存在（`--version` / `-V`）。
    auto request_version() -> void {
        early_ = ParseOutcome::Version;
        early_text_ = version_text(*matched(), program_);
    }

    /// @brief 以 `-` 开头的 token：长选项或短选项簇。
    [[nodiscard]] auto consume_option_token(std::string_view token) -> std::optional<Error> {
        if (token.starts_with("--")) {
            return consume_long(token);
        }
        return consume_short(token);
    }

    [[nodiscard]] auto consume_long(std::string_view token) -> std::optional<Error> {
        const std::string_view body = token.substr(2);
        const auto equals = body.find('=');
        const bool inline_value = (equals != std::string_view::npos);
        const std::string_view name = inline_value ? body.substr(0, equals) : body;
        const OptionSchema *spec = matched()->find_option(name);
        if (spec == nullptr) {
            std::vector<std::string> candidates;
            for (const auto &option : matched()->options) {
                candidates.push_back(option.long_name);
            }
            for (const auto &sub : matched()->subcommands) {
                candidates.push_back(sub.name);
            }
            const std::string guess = closest_candidate(name, candidates);
            auto error = make_error(ErrorCode::CliUnknownOption,
                                    ErrorParams{{"option", display_long(name)}, {"command", command_display()}});
            if (!guess.empty()) {
                error.suggestion = "Did you mean " + display_long(guess) + "?";
            }
            return error;
        }
        if (inline_value) {
            return store(*spec, body.substr(equals + 1));
        }
        return consume_value_tokens(*spec);
    }

    /// @brief 短选项簇：`-v`、`-vw 80`、`-w80`、`-w=80`。
    [[nodiscard]] auto consume_short(std::string_view token) -> std::optional<Error> {
        const std::string_view body = token.substr(1);
        for (std::size_t i = 0; i < body.size(); ++i) {
            const char letter = body[i];
            if (letter == 'h' && matched()->find_short('h') == nullptr) {
                request_help();
                return std::nullopt;
            }
            if (letter == 'V' && matched()->find_short('V') == nullptr && !matched()->version.empty()) {
                request_version();
                return std::nullopt;
            }
            const OptionSchema *spec = matched()->find_short(letter);
            if (spec == nullptr) {
                std::vector<std::string> candidates;
                for (const auto &option : matched()->options) {
                    if (option.short_name != '\0') {
                        candidates.emplace_back(1, option.short_name);
                    }
                }
                const std::string guess = closest_candidate(std::string_view{&letter, 1}, candidates);
                auto error = make_error(ErrorCode::CliUnknownOption, ErrorParams{{"option", std::string{'-', letter}},
                                                                                 {"command", command_display()}});
                if (!guess.empty()) {
                    error.suggestion = "Did you mean -" + guess + "?";
                }
                return error;
            }
            const std::string_view remainder = body.substr(i + 1);
            const bool takes_no_value = (spec->kind == ValueKind::Bool) || spec->arity.max == 0;
            if (takes_no_value && (remainder.empty() || remainder.front() != '=')) {
                if (auto error = store(*spec, "true"); error) {
                    return error;
                }
                continue;  // 簇内继续解析下一个短名
            }
            if (!remainder.empty()) {
                return store(*spec, remainder.front() == '=' ? remainder.substr(1) : remainder);
            }
            return consume_value_tokens(*spec);
        }
        return std::nullopt;
    }

    /// @brief 定长 arity 精确吞 n 个后续 token；变长 arity 先吞 min 再尽可能多吞。
    [[nodiscard]] auto consume_value_tokens(const OptionSchema &spec) -> std::optional<Error> {
        if (spec.kind == ValueKind::Bool || spec.arity.max == 0) {
            return store(spec, "true");
        }
        if (const auto span = spec.arity.fixed_span(); span && *span > 0) {
            for (int k = 0; k < *span; ++k) {
                const auto next = peek_value_token();
                if (!next) {
                    return arity_or_missing_value(spec, k);
                }
                if (auto error = store(spec, *next); error) {
                    return error;
                }
                advance();
            }
            return std::nullopt;
        }
        int taken = 0;
        while (const auto next = peek_value_token()) {
            if (auto error = store(spec, *next); error) {
                return error;
            }
            advance();
            ++taken;
            if (spec.arity.max != Arity::AURORA_UNBOUNDED && taken >= spec.arity.max) {
                break;
            }
        }
        if (taken < spec.arity.min) {
            return arity_error(spec, taken);
        }
        return std::nullopt;
    }

    [[nodiscard]] auto arity_or_missing_value(const OptionSchema &spec, int already_taken) -> std::optional<Error> {
        if (already_taken == 0 && spec.arity.min <= 0) {
            return std::nullopt;  // min==0 的变长项允许「出现但无值」
        }
        if (already_taken == 0) {
            return make_error(ErrorCode::CliMissingValue, ErrorParams{{"option", display_long(spec.long_name)}});
        }
        return arity_error(spec, already_taken);
    }

    [[nodiscard]] auto arity_error(const OptionSchema &spec, int actual) -> std::optional<Error> {
        return make_error(ErrorCode::CliArityViolated, ErrorParams{{"option", display_long(spec.long_name)},
                                                                   {"min", std::to_string(spec.arity.min)},
                                                                   {"max", spec.arity.max_text()},
                                                                   {"actual", std::to_string(actual)}});
    }

    /// @brief 下一个可作值的 token；被选项样式占据或耗尽时返回 nullopt。
    [[nodiscard]] auto peek_value_token() const -> std::optional<std::string_view> {
        if (cursor_ + 1 >= tokens_.size()) {
            return std::nullopt;
        }
        const std::string_view next = tokens_[cursor_ + 1];
        if (looks_like_option(next) || next == "--") {
            return std::nullopt;
        }
        return next;
    }

    auto advance() -> void { ++cursor_; }

    /// @brief 转换 + 取值域校验 + 登记一个选项值。
    [[nodiscard]] auto store(const OptionSchema &spec, std::string_view token) -> std::optional<Error> {
        auto converted = detail::convert_literal(spec.kind, token, display_long(spec.long_name));
        if (!converted) {
            return converted.error();
        }
        if (auto error = check_domain(spec, converted.value())) {
            return error;
        }
        auto &slots = levels_.back().slots;
        auto slot = std::find_if(slots.begin(), slots.end(),
                                 [&](const Arguments::Slot &s) { return s.spec->long_name == spec.long_name; });
        if (slot == slots.end()) {
            slots.push_back(Arguments::Slot{.spec = &spec, .given = true});
            slot = std::prev(slots.end());
        }
        slot->values.push_back(converted.value());
        slot->given = true;
        const int occurrences = static_cast<int>(slot->values.size());
        // Bool 的出现次数就是计数器（`-vvv`），其 arity 描述的是「消耗几个值 token」（0 个），
        // 故不能拿它当重复上限；非 Bool 的 `{1,1}` 才把重复出现判为 arity 违规。
        if (spec.kind != ValueKind::Bool && spec.arity.max != Arity::AURORA_UNBOUNDED && occurrences > spec.arity.max) {
            return arity_error(spec, occurrences);
        }
        return std::nullopt;
    }

    /// @brief choices / minimum / maximum 取值域检查。
    [[nodiscard]] auto check_domain(const OptionSchema &spec, const Value &value) -> std::optional<Error> {
        const std::string option = display_long(spec.long_name);
        if (!spec.choices.empty() &&
            std::find(spec.choices.begin(), spec.choices.end(), value.raw_text()) == spec.choices.end()) {
            auto error =
                make_error(ErrorCode::CliChoiceInvalid, ErrorParams{{"option", option}, {"value", value.raw_text()}});
            error.suggestion = "Allowed values: " + join_comma(spec.choices);
            return error;
        }
        if (!spec.minimum && !spec.maximum) {
            return std::nullopt;
        }
        const auto number = value.as_double();
        if (!number) {
            return std::nullopt;  // 非数值类型不参与区间判定（validate 已拦，此处宽容）
        }
        const double low = spec.minimum.value_or(-std::numeric_limits<double>::infinity());
        const double high = spec.maximum.value_or(std::numeric_limits<double>::infinity());
        if (number.value() < low || number.value() > high) {
            return make_error(ErrorCode::CliRangeViolated,
                              ErrorParams{{"option", option},
                                          {"value", value.raw_text()},
                                          {"min", spec.minimum ? detail::bound_text(*spec.minimum) : "-inf"},
                                          {"max", spec.maximum ? detail::bound_text(*spec.maximum) : "inf"}});
        }
        return std::nullopt;
    }

    /// @brief 裸 token：子命令优先下钻，其次位置参数，最后报错。
    [[nodiscard]] auto consume_bare_token(std::string_view token) -> std::optional<Error> {
        auto &level = levels_.back();
        const CommandSpec *spec = level.spec;
        // 「精确命中子命令名」优先于位置参数槽（git / cobra / npm 同规则）：否则根命令的变长
        // 位置参数会把 `render` 吞成一条普通取值，整棵子命令树永远进不去。
        const CommandSpec *sub = spec->find_subcommand(token);
        if (sub != nullptr) {
            levels_.push_back(Level{.spec = sub});
            return std::nullopt;
        }
        if (resolve_positional(*spec, level.positional_cursor) == nullptr) {
            if (!spec->subcommands.empty()) {
                std::vector<std::string> candidates;
                candidates.reserve(spec->subcommands.size());
                for (const auto &child : spec->subcommands) {
                    candidates.push_back(child.name);
                }
                const std::string guess = closest_candidate(token, candidates);
                auto error = make_error(ErrorCode::CliUnknownSubcommand, ErrorParams{{"subcommand", std::string{token}},
                                                                                     {"command", command_display()}});
                error.suggestion =
                    guess.empty() ? ("Available: " + join_comma(candidates)) : ("Did you mean " + guess + "?");
                return error;
            }
            return make_error(ErrorCode::CliTooManyPositionals,
                              ErrorParams{{"command", command_display()}, {"value", std::string{token}}});
        }
        return take_positional(token, false);
    }

    /// @brief 把 token 灌进当前位置参数槽；`--` 之后溢出到 rest。
    [[nodiscard]] auto take_positional(std::string_view token, bool trailing) -> std::optional<Error> {
        auto &level = levels_.back();
        const PositionalSchema *slot = resolve_positional(*level.spec, level.positional_cursor);
        if (slot == nullptr) {
            if (trailing) {
                rest_.emplace_back(token);
                return std::nullopt;
            }
            return make_error(ErrorCode::CliTooManyPositionals,
                              ErrorParams{{"command", command_display()}, {"value", std::string{token}}});
        }
        auto converted = detail::convert_literal(slot->kind, token, slot->name);
        if (!converted) {
            return make_error(ErrorCode::CliInvalidValue, ErrorParams{{"option", slot->name},
                                                                      {"kind", std::string{to_string(slot->kind)}},
                                                                      {"value", std::string{token}}});
        }
        if (!slot->choices.empty() &&
            std::find(slot->choices.begin(), slot->choices.end(), token) == slot->choices.end()) {
            auto error = make_error(ErrorCode::CliChoiceInvalid,
                                    ErrorParams{{"option", slot->name}, {"value", std::string{token}}});
            error.suggestion = "Allowed values: " + join_comma(slot->choices);
            return error;
        }
        level.positionals.push_back(converted.value());
        ++level.positional_cursor;
        return std::nullopt;
    }

    /// @brief 位置参数槽解析：变长槽吸收其余 token（validate 保证变长在末位）。
    [[nodiscard]] static auto resolve_positional(const CommandSpec &spec, std::size_t cursor)
        -> const PositionalSchema * {
        std::size_t acc = 0;
        for (const auto &slot : spec.positionals) {
            if (slot.arity.max == Arity::AURORA_UNBOUNDED) {
                return (cursor >= acc + static_cast<std::size_t>(slot.arity.min)) ? &slot : nullptr;
            }
            if (cursor < acc + static_cast<std::size_t>(slot.arity.max)) {
                return &slot;
            }
            acc += static_cast<std::size_t>(slot.arity.max);
        }
        return nullptr;
    }

    /// @brief 收尾：逐层补默认值，检查必填与互斥，产出合并槽位（叶命令优先）。
    [[nodiscard]] auto finish() -> std::vector<Arguments::Slot> {
        for (auto &level : levels_) {
            materialize_option_defaults(level);
            materialize_positional_defaults(level);
        }
        check_required();
        auto merged = merge_slots();
        check_conflicts(merged);
        return merged;
    }

    auto materialize_option_defaults(Level &level) -> void {
        for (const auto &option : level.spec->options) {
            auto slot = std::find_if(level.slots.begin(), level.slots.end(),
                                     [&](const Arguments::Slot &s) { return s.spec->long_name == option.long_name; });
            if ((slot != level.slots.end() && slot->given) || option.default_text.empty()) {
                continue;
            }
            auto converted = detail::convert_literal(option.kind, option.default_text, display_long(option.long_name));
            if (!converted || check_domain(option, converted.value())) {
                continue;  // 非法默认值由 validate 负责报错，解析期静默跳过
            }
            if (slot == level.slots.end()) {
                level.slots.push_back(Arguments::Slot{.spec = &option, .values = {converted.value()}});
            } else {
                slot->values = {converted.value()};
                slot->given = false;
            }
        }
    }

    auto materialize_positional_defaults(Level &level) -> void {
        std::size_t flat = 0;
        for (const auto &slot : level.spec->positionals) {
            const std::size_t width = (slot.arity.max == Arity::AURORA_UNBOUNDED)
                                          ? static_cast<std::size_t>(std::max(slot.arity.min, 0))
                                          : static_cast<std::size_t>(slot.arity.max);
            for (std::size_t k = 0; k < width; ++k, ++flat) {
                if (flat < level.positional_cursor || slot.default_text.empty()) {
                    continue;
                }
                auto converted = detail::convert_literal(slot.kind, slot.default_text, slot.name);
                if (!converted) {
                    continue;
                }
                level.positionals.push_back(converted.value());
                level.positional_cursor = flat + 1;
            }
        }
    }

    auto check_required() -> void {
        for (const auto &level : levels_) {
            const auto &spec = *level.spec;
            for (const auto &option : spec.options) {
                if (!option.required) {
                    continue;
                }
                const auto slot = std::find_if(level.slots.begin(), level.slots.end(),
                                               [&](const Arguments::Slot &s) { return s.spec == &option; });
                if (slot != level.slots.end() && slot->given) {
                    continue;
                }
                pending(make_error(
                    ErrorCode::CliMissingRequired,
                    ErrorParams{{"option", display_long(option.long_name)}, {"command", command_display()}}));
                return;
            }
            if (spec.subcommand_required && levels_.size() == 1) {
                pending(make_error(ErrorCode::CliMissingSubcommand, ErrorParams{{"command", command_display()}}));
                return;
            }
            std::size_t flat = 0;
            for (const auto &slot : spec.positionals) {
                const std::size_t width = (slot.arity.max == Arity::AURORA_UNBOUNDED)
                                              ? static_cast<std::size_t>(std::max(slot.arity.min, 0))
                                              : static_cast<std::size_t>(slot.arity.max);
                for (std::size_t k = 0; k < width; ++k, ++flat) {
                    if (flat >= level.positional_cursor && slot.arity.min > 0 && slot.default_text.empty()) {
                        pending(make_error(ErrorCode::CliMissingRequired,
                                           ErrorParams{{"option", slot.name}, {"command", command_display()}}));
                        return;
                    }
                }
            }
        }
    }

    /// @brief 按长名合并各层槽位：叶命令优先，但「优先」只针对显式给出的取值。
    ///
    /// 命令树允许父子声明同名选项（`aurora-render --format json render` 与
    /// `aurora-render render --format webp`），合并口径取「最深显式给出者胜出」；
    /// 叶层仅物化了默认值时，不得抹掉父级用户真写过的取值——否则输入静默丢失。
    [[nodiscard]] auto merge_slots() -> std::vector<Arguments::Slot> {
        std::vector<Arguments::Slot> merged;
        for (auto &level : levels_ | std::views::reverse) {  // 叶优先
            for (auto &slot : level.slots) {
                const auto hit = std::find_if(merged.begin(), merged.end(), [&](const Arguments::Slot &s) {
                    return s.spec->long_name == slot.spec->long_name;
                });
                if (hit == merged.end()) {
                    merged.push_back(std::move(slot));
                } else if (!hit->given && slot.given) {
                    *hit = std::move(slot);  // 父级显式值覆盖叶级默认值
                }
            }
        }
        return merged;
    }

    auto check_conflicts(const std::vector<Arguments::Slot> &slots) -> void {
        for (const auto &slot : slots) {
            if (!slot.given) {
                continue;
            }
            for (const auto &other : slots) {
                if (!other.given || other.spec == slot.spec) {
                    continue;
                }
                const auto hit = std::find(slot.spec->conflicts_with.begin(), slot.spec->conflicts_with.end(),
                                           other.spec->long_name);
                if (hit != slot.spec->conflicts_with.end()) {
                    pending(make_error(ErrorCode::CliConflictViolated,
                                       ErrorParams{{"option", display_long(slot.spec->long_name)},
                                                   {"conflict", display_long(other.spec->long_name)}}));
                    return;
                }
            }
        }
    }

    const CommandSpec *root_ = nullptr;
    std::span<const std::string_view> tokens_;
    std::vector<Level> levels_;
    std::vector<std::string> rest_;
    std::string program_;
    std::size_t cursor_ = 0;
    std::optional<ParseOutcome> early_;
    std::string early_text_;
    std::optional<Error> pending_;
};

// ------------------------------------------------------------ Value

auto Value::as_bool() const -> Result<bool> {
    if (const auto *flag = std::get_if<bool>(&raw_)) {
        return *flag;
    }
    return detail::invalid_literal(raw_text_, ValueKind::Bool, raw_text_);
}

auto Value::as_int64() const -> Result<std::int64_t> {
    if (const auto *number = std::get_if<std::int64_t>(&raw_)) {
        return *number;
    }
    if (const auto *wide = std::get_if<double>(&raw_)) {
        return narrow_or_ok<std::int64_t>(*this, ValueKind::Int, *wide);
    }
    return detail::invalid_literal(raw_text_, ValueKind::Int, raw_text_);
}

auto Value::as_int() const -> Result<int> {
    auto wide = as_int64();
    if (!wide) {
        return wide.error();
    }
    const std::int64_t value = wide.value();
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return make_error(ErrorCode::CliRangeViolated,
                          ErrorParams{{"option", raw_text_},
                                      {"value", std::to_string(value)},
                                      {"min", std::to_string(std::numeric_limits<int>::min())},
                                      {"max", std::to_string(std::numeric_limits<int>::max())}});
    }
    return static_cast<int>(value);
}

auto Value::as_double() const -> Result<double> {
    if (const auto *number = std::get_if<double>(&raw_)) {
        return *number;
    }
    if (const auto *integer = std::get_if<std::int64_t>(&raw_)) {
        return static_cast<double>(*integer);
    }
    return detail::invalid_literal(raw_text_, ValueKind::Double, raw_text_);
}

auto Value::as_string() const -> Result<std::string> {
    if (const auto *text = std::get_if<std::string>(&raw_)) {
        return *text;
    }
    return detail::invalid_literal(raw_text_, ValueKind::String, raw_text_);
}

auto Value::as_length() const -> Result<Length> {
    if (const auto *length = std::get_if<Length>(&raw_)) {
        return *length;
    }
    return detail::invalid_literal(raw_text_, ValueKind::Length, raw_text_);
}

auto Value::as_color() const -> Result<Color> {
    if (const auto *color = std::get_if<Color>(&raw_)) {
        return *color;
    }
    return detail::invalid_literal(raw_text_, ValueKind::Color, raw_text_);
}

auto Value::as_log_level() const -> Result<LogLevel> {
    if (const auto *level = std::get_if<LogLevel>(&raw_)) {
        return *level;
    }
    return detail::invalid_literal(raw_text_, ValueKind::LogLevel, raw_text_);
}

auto Value::as_duration_ms() const -> Result<std::int64_t> {
    if (kind_ != ValueKind::Duration) {
        return detail::invalid_literal(raw_text_, ValueKind::Duration, raw_text_);
    }
    return as_int64();
}

// ------------------------------------------------------------ Arguments

auto Arguments::find_slot(std::string_view long_name) const -> const Slot * {
    for (const auto &slot : slots_) {
        if (slot.spec->long_name == long_name) {
            return &slot;
        }
    }
    return nullptr;
}

auto Arguments::values(std::string_view long_name) const -> std::vector<Value> {
    const auto *slot = find_slot(long_name);
    return (slot == nullptr) ? std::vector<Value>{} : slot->values;
}

auto Arguments::value(std::string_view long_name) const -> Result<Value> {
    const auto *slot = find_slot(long_name);
    if (slot == nullptr || slot->values.empty()) {
        return make_error(ErrorCode::CliMissingRequired,
                          ErrorParams{{"option", display_long(long_name)}, {"command", command_display()}});
    }
    if (slot->values.size() > 1U) {
        return make_error(ErrorCode::CliArityViolated, ErrorParams{{"option", display_long(long_name)},
                                                                   {"min", std::to_string(slot->spec->arity.min)},
                                                                   {"max", slot->spec->arity.max_text()},
                                                                   {"actual", std::to_string(slot->values.size())}});
    }
    return slot->values.front();
}

auto Arguments::flag(std::string_view long_name) const -> bool {
    const auto *slot = find_slot(long_name);
    if (slot == nullptr || slot->values.empty()) {
        return false;
    }
    if (const auto *value = std::get_if<bool>(&slot->values.front().raw())) {
        return *value;
    }
    return slot->given;
}

auto Arguments::count(std::string_view long_name) const -> int {
    const auto *slot = find_slot(long_name);
    return (slot == nullptr || !slot->given) ? 0 : static_cast<int>(slot->values.size());
}

auto Arguments::explicitly_given(std::string_view long_name) const -> bool {
    const auto *slot = find_slot(long_name);
    return slot != nullptr && slot->given;
}

auto Arguments::positional(std::size_t index) const -> Result<Value> {
    if (index >= positionals_.size()) {
        return make_error(
            ErrorCode::CliMissingRequired,
            ErrorParams{{"option", "positional[" + std::to_string(index) + "]"}, {"command", command_display()}});
    }
    return positionals_[index];
}

auto Arguments::command_display() const -> std::string {
    std::string out;
    for (const auto &name : chain_) {
        if (!out.empty()) {
            out += ' ';
        }
        out += name;
    }
    return out;
}

// ------------------------------------------------------------ 值类型与 arity

namespace {
/// @brief 全部取值种类，顺序与 `ValueKind` 的声明顺序一致。
constexpr std::array<ValueKind, 9> AURORA_ALL_VALUE_KINDS{ValueKind::Bool,   ValueKind::Int,      ValueKind::Double,
                                                          ValueKind::String, ValueKind::Enum,     ValueKind::Length,
                                                          ValueKind::Color,  ValueKind::LogLevel, ValueKind::Duration};
}  // namespace

auto all_value_kinds() -> std::vector<ValueKind> {
    return std::vector<ValueKind>{AURORA_ALL_VALUE_KINDS.begin(), AURORA_ALL_VALUE_KINDS.end()};
}

auto to_string(ValueKind kind) noexcept -> std::string_view {
    switch (kind) {
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Int:
            return "int";
        case ValueKind::Double:
            return "double";
        case ValueKind::String:
            return "string";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Length:
            return "length";
        case ValueKind::Color:
            return "color";
        case ValueKind::LogLevel:
            return "log-level";
        case ValueKind::Duration:
            return "duration";
    }
    return "unknown";
}

auto value_kind_from_name(std::string_view name) noexcept -> std::optional<ValueKind> {
    // 走 constexpr 表而非 all_value_kinds()：后者返回 vector，在 noexcept 函数里即抛出路径。
    for (const auto kind : AURORA_ALL_VALUE_KINDS) {
        if (to_string(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

auto Arity::max_text() const -> std::string {
    return (max == Arity::AURORA_UNBOUNDED) ? std::string{"∞"} : std::to_string(max);
}

auto outcome_to_string(ParseOutcome outcome) noexcept -> std::string_view {
    switch (outcome) {
        case ParseOutcome::Ok:
            return "ok";
        case ParseOutcome::Help:
            return "help";
        case ParseOutcome::Version:
            return "version";
    }
    return "ok";
}

// ------------------------------------------------------------ parse 入口

auto parse(const CommandSpec &root, std::span<const std::string_view> tokens, std::string_view program_name)
    -> Result<Invocation> {
    return Parser{root, tokens, program_name}.run();
}

auto parse(const CommandSpec &root, const std::vector<std::string> &tokens, std::string_view program_name)
    -> Result<Invocation> {
    std::vector<std::string_view> views;
    views.reserve(tokens.size());
    for (const auto &token : tokens) {
        views.emplace_back(token);
    }
    return parse(root, std::span<const std::string_view>{views}, program_name);
}

auto parse(const CommandSpec &root, int argc, const char *const *argv) -> Result<Invocation> {
    // 以 span 而非裸下标读 argv：argc<=0 / argv==nullptr 一律收敛成空视图，无指针算术。
    std::span<const char *const> args{};
    if (argc > 0 && argv != nullptr) {
        args = {argv, static_cast<std::size_t>(argc)};
    }
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const auto *token : args.subspan(args.empty() ? 0 : 1)) {
        views.emplace_back(token);
    }
    std::string program_name;
    if (!args.empty() && args.front() != nullptr) {
        const std::string_view raw{args.front()};
        // find_last_of 而非 max(rfind, rfind)：任一分隔符缺席时 rfind 返回 npos，而 npos 是
        // size_t 最大值，max 会永远选中它，导致另一条分支失效。
        const auto slash = raw.find_last_of("/\\");
        program_name = (slash == std::string_view::npos) ? std::string{raw} : std::string{raw.substr(slash + 1)};
    }
    return parse(root, std::span<const std::string_view>{views}, program_name);
}

}  // namespace aurora::cli
