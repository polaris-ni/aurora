// http_cache.cpp —— 内存缓存的判定与表（见同名头的文件头口径）。
#include "aurora/net/detail/http_cache.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/net/detail/http_retry.h"

namespace aurora::net::detail {
namespace {

/// @brief 去掉首尾 OWS（空格与水平制表）。
[[nodiscard]] auto trim_ows(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief ASCII 小写化（字段名与指令名一律按此归一，不做 Unicode 折叠）。
[[nodiscard]] auto to_lower_ascii(std::string_view text) -> std::string {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) {
        const auto raw = static_cast<unsigned char>(c);
        return (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw + 32) : c;
    });
    return out;
}

/// @brief 恒参与建键的凭据字段：应答有没有说 `Vary` 都要收进条目（纪律见 `net/http_auth.h` 的凭据纪律条）。
constexpr std::array<std::string_view, 2> AURORA_CREDENTIAL_VARY_FIELDS{std::string_view{"Authorization"},
                                                                        std::string_view{"Cookie"}};

/// @brief 字段名比较：ASCII 大小写折叠，与 `aurora::Headers` 的查找口径同一条尺。
/// @param left 一侧名字。
/// @param right 另一侧名字。
/// @return 折叠后逐字节相同则为 true。
[[nodiscard]] auto ascii_name_equal(std::string_view left, std::string_view right) -> bool {
    return to_lower_ascii(left) == to_lower_ascii(right);
}

/// @brief 解析非负秒数（`max-age` / `Age` 共用）；带符号、带小数、混字母一律判「没给」。
[[nodiscard]] auto parse_seconds(std::string_view text) -> std::optional<std::chrono::seconds> {
    const std::string_view digits = trim_ows(text);
    if (digits.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char *const first = digits.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 需要 [first, last)
    const char *const last = first + digits.size();
    const auto parsed = std::from_chars(first, last, value);
    // 读不到尾即不是「一整串数字」：带符号（负数）、混字母、小数点后的残余都在这里落网，按「没给」处理。
    if (parsed.ptr != last) {
        return std::nullopt;
    }
    // 饱和到 `int64` 可表示范围：对端给一枚天文数字的 `max-age`，意思就是「很久不用问」，没必要绕成负数；
    // 纯数字只是超出可表示范围也不当「没给」——那样会把一条明明给了有效期的应答打回「每次都要验证」。
    constexpr std::uint64_t saturation = 9223372036854775807ULL;
    if (parsed.ec == std::errc::result_out_of_range) {
        return std::chrono::seconds{static_cast<std::int64_t>(saturation)};
    }
    if (parsed.ec != std::errc{}) {
        return std::nullopt;
    }
    return std::chrono::seconds{static_cast<std::int64_t>(std::min(value, saturation))};
}

/// @brief 取一枚可解析为日期头的字段（`Date` / `Expires`），走 `parse_http_date()` 的纯日历算术。
[[nodiscard]] auto date_field(const aurora::Headers &headers, std::string_view name) -> std::optional<CacheTime> {
    const auto text = headers.get(name);
    if (!text.has_value()) {
        return std::nullopt;
    }
    return parse_http_date(trim_ows(text.value()));
}

/// @brief 两时刻之间的秒数（右减左，可为负）。
[[nodiscard]] auto seconds_between(CacheTime from, CacheTime to) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(to - from).count();
}

/// @brief 304 不许覆盖的那四枚字段——它们描述的是载荷本体，而载荷本体来自已存那份。
[[nodiscard]] auto is_payload_describing(std::string_view lower_name) -> bool {
    return lower_name == "content-length" || lower_name == "content-range" || lower_name == "trailer" ||
           lower_name == "transfer-encoding";
}

/// @brief 按逗号切 `Cache-Control`，跳过双引号内的逗号（`token="a,b"` 是合法写法）。
[[nodiscard]] auto split_directives(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t begin = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            out.push_back(text.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    out.push_back(text.substr(begin));
    return out;
}

/// @brief 从表里的一份应答取出交付形态并标记「出自缓存」。
[[nodiscard]] auto cached_copy(const CacheEntry &entry) -> aurora::net::http::Response {
    return aurora::net::http::Response{entry.response.status(), std::string{entry.response.reason()},
                                       entry.response.headers(), entry.response.body(), true};
}

}  // namespace

auto joined_field(const aurora::Headers &headers, std::string_view name) -> std::string {
    std::string out;
    for (const auto &value : headers.values(name)) {
        if (!out.empty()) {
            out += ',';
        }
        out += value;
    }
    return out;
}

auto parse_cache_control(std::string_view value) -> CacheDirectives {
    CacheDirectives out;
    for (const auto raw : split_directives(value)) {
        const std::string_view item = trim_ows(raw);
        if (item.empty()) {
            continue;
        }
        const auto equals = item.find('=');
        // 指令名两侧的 OWS 一并吃掉：严格文法（`RFC 9111 §5.2`）不许 `max-age = 60` 里那两枚空格，但一颗
        // 空格不该让一条本来能缓存的应答失去新鲜期——那是对端的排版问题，不是本库的判定问题。
        const std::string name =
            to_lower_ascii(trim_ows(equals == std::string_view::npos ? item : item.substr(0, equals)));
        std::string_view argument = equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1);
        argument = trim_ows(argument);
        if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"') {
            argument = argument.substr(1, argument.size() - 2);
        }
        if (name == "no-store") {
            out.no_store = true;
        } else if (name == "no-cache") {
            out.no_cache = true;
        } else if (name == "must-revalidate") {
            out.must_revalidate = true;
        } else if (name == "max-age") {
            // 同份里出现多次取最后一个：后写覆盖先写，与字段的书写次序一致。
            if (const auto parsed = parse_seconds(argument)) {
                out.max_age = parsed;
            }
        }
    }
    return out;
}

auto is_cacheable_status(std::uint16_t status) -> bool {
    static constexpr std::uint16_t CACHEABLE[] = {200, 203, 300, 301, 404, 405, 410, 414, 501};
    return std::ranges::find(CACHEABLE, status) != std::end(CACHEABLE);
}

auto is_cacheable_method(aurora::net::http::Method method) -> bool {
    return method == aurora::net::http::Method::Get || method == aurora::net::http::Method::Head;
}

auto parse_vary(std::string_view value) -> std::optional<std::vector<std::string>> {
    std::vector<std::string> names;
    bool wildcard = false;
    for (const auto raw : split_directives(value)) {
        const std::string_view item = trim_ows(raw);
        if (item.empty()) {
            continue;
        }
        if (item == "*") {
            wildcard = true;
            continue;
        }
        const std::string lowered = to_lower_ascii(item);
        if (std::ranges::find(names, lowered) == names.end()) {
            names.push_back(lowered);
        }
    }
    // `Vary: *` 意为「随任何请求特征而变」，与「没给 Vary」是两回事，故用 nullopt 而非空列表表达。
    if (wildcard) {
        return std::nullopt;
    }
    return names;
}

auto freshness_of(const aurora::net::http::Response &response, CacheTime arrived) -> Freshness {
    const auto directives = parse_cache_control(joined_field(response.headers(), "Cache-Control"));
    return freshness_of(response, arrived, directives);
}

auto freshness_of(const aurora::net::http::Response &response, CacheTime arrived, const CacheDirectives &directives)
    -> Freshness {
    Freshness out;
    out.arrived = arrived;

    // 初始年龄：`Date` 说的时刻与抵达时刻差出来的那截，与中间代理累计的 `Age` 取大（请求滞留按 0 计）。
    const auto date = date_field(response.headers(), "Date");
    std::int64_t apparent_age = 0;
    if (date.has_value()) {
        apparent_age = std::max<std::int64_t>(0, seconds_between(date.value(), arrived));
    }
    std::int64_t reported_age = 0;
    if (const auto age = response.header("Age"); age.has_value()) {
        if (const auto parsed = parse_seconds(age.value())) {
            reported_age = parsed->count();
        }
    }
    out.initial_age = std::max(apparent_age, reported_age);

    // 新鲜期只认显式有效期：`max-age` 优先，其次 `Expires` 减基准时刻；两者皆无即 0（即刻过期）。
    if (directives.max_age.has_value()) {
        out.lifetime = directives.max_age.value();
    } else if (const auto expires = date_field(response.headers(), "Expires"); expires.has_value()) {
        const CacheTime base = date.value_or(arrived);
        out.lifetime = std::chrono::seconds{std::max<std::int64_t>(0, seconds_between(base, expires.value()))};
    } else {
        out.lifetime = std::chrono::seconds{0};
    }
    if (directives.no_cache) {
        out.lifetime = std::chrono::seconds{0};  // 响应侧 `no-cache`：可以存，但交付前必须问过对端
    }
    return out;
}

auto is_fresh(const CacheEntry &entry, CacheTime now) -> bool {
    const std::int64_t resident = std::max<std::int64_t>(0, seconds_between(entry.freshness.arrived, now));
    // 加法前先夹住上限：`initial_age` 可以是被饱和过的天文数字（对端给一枚超范围的 `Age`），
    // 直接相加会溢出成负数——那会把一条早已老掉牙的条目读成「刚刚抵达」。
    constexpr std::int64_t ceiling = std::numeric_limits<std::int64_t>::max();
    const std::int64_t current_age =
        resident > ceiling - entry.freshness.initial_age ? ceiling : entry.freshness.initial_age + resident;
    return current_age < entry.freshness.lifetime.count();
}

auto merge_revalidated_headers(const aurora::Headers &stored, const aurora::Headers &update) -> aurora::Headers {
    aurora::Headers out = stored;
    std::vector<std::string> handled;  // 同一枚字段名只并一次，避免多行同名把顺序换来换去
    for (const auto &entry : update.entries()) {
        const std::string lowered = to_lower_ascii(entry.name);
        if (is_payload_describing(lowered)) {
            continue;
        }
        if (std::ranges::find(handled, lowered) != handled.end()) {
            continue;
        }
        handled.push_back(lowered);
        // 同名的多枚一并带上（`Set-Cookie` 这类多值字段不因 304 只给一枚而缩水成一条）。
        const auto values = update.values(entry.name);
        if (values.size() > 1) {
            out.remove(entry.name);
            for (const auto &value : values) {
                out.append(entry.name, value);
            }
            continue;
        }
        out.set(entry.name, entry.value);
    }
    return out;
}

HttpResponseCache::HttpResponseCache(aurora::net::http::CacheOptions options) : options_{options} {}

auto HttpResponseCache::set_options(aurora::net::http::CacheOptions options) -> void {
    const std::scoped_lock guard{mutex_};
    options_ = options;
    evict_over_capacity();
}

auto HttpResponseCache::lookup(aurora::net::http::Method method, const std::string &url,
                               const aurora::Headers &request_headers, CacheTime now) -> CacheLookup {
    CacheLookup out;
    const std::scoped_lock guard{mutex_};
    const auto index = find_index(method, url, request_headers);
    if (!index.has_value()) {
        return out;
    }
    auto &entry = entries_[index.value()];
    entry.used = ++clock_;
    if (is_fresh(entry, now)) {
        out.fresh = cached_copy(entry);
        return out;
    }
    if (entry.etag.has_value() || entry.last_modified.has_value()) {
        out.revalidate = entry;
    }
    return out;
}

auto HttpResponseCache::store(aurora::net::http::Method method, const std::string &url,
                              const aurora::Headers &request_headers, const aurora::net::http::Response &response,
                              CacheTime now) -> bool {
    if (!is_cacheable_method(method) || !is_cacheable_status(response.status())) {
        return false;
    }
    const auto stored_directives = parse_cache_control(joined_field(response.headers(), "Cache-Control"));
    if (stored_directives.no_store) {
        return false;
    }
    const auto request_directives = parse_cache_control(joined_field(request_headers, "Cache-Control"));
    if (request_directives.no_store) {
        return false;  // 调用方在请求侧就说了别存
    }
    const auto vary_names = parse_vary(joined_field(response.headers(), "Vary"));
    if (!vary_names.has_value()) {
        return false;  // `Vary: *`：不可复用
    }

    auto freshness = freshness_of(response, now, stored_directives);
    if (request_directives.no_cache) {
        freshness.lifetime = std::chrono::seconds{0};  // 请求侧 `no-cache`：存可以，交付前得再问一次
    }
    const auto etag = response.header("ETag");
    const auto last_modified = response.header("Last-Modified");
    if (freshness.lifetime.count() <= 0 && !etag.has_value() && !last_modified.has_value()) {
        return false;  // 既没有效期又没法重验证：存了只能靠启发式交付，本库不做
    }

    CacheEntry entry;
    entry.method = method;
    entry.url = url;
    // 凭据恒参与建键，与应答有没有写 `Vary` 无关（那条纪律见 `net/http_auth.h`）：一份带 `Authorization`
    // 取回的副本若存成裸键，下一次裸打的请求就能读到它——那等于把上一位调用方的凭据换回的私密内容交给
    // 了一位没有凭据的调用方。缺省也算一种值（空串），于是「没带凭据取回的副本」也只对没带凭据的请求可见。
    // 复用同一枚 (name, value) 形态而不是另造一维键，是为了让命中判定仍只有一处（`find_index`）。
    entry.vary.reserve(vary_names->size() + AURORA_CREDENTIAL_VARY_FIELDS.size());
    for (const auto &name : vary_names.value()) {
        entry.vary.emplace_back(name, request_headers.get(name).value_or(std::string{}));
    }
    for (const auto &name : AURORA_CREDENTIAL_VARY_FIELDS) {
        if (std::ranges::any_of(*vary_names,
                                [&name](const std::string &given) { return ascii_name_equal(given, name); })) {
            continue;  // 对端已经点名它：上面那条循环按同一枚值收过了，收两遍只会让条目多一处同值比较
        }
        entry.vary.emplace_back(name, request_headers.get(name).value_or(std::string{}));
    }
    // 表里存的是「未标缓存」的形态：`from_cache()` 只在交付那一刻置真，否则条目自己会被读成一次命中。
    entry.response = aurora::net::http::Response{response.status(), std::string{response.reason()}, response.headers(),
                                                 response.body(), false};
    entry.freshness = freshness;
    entry.etag = etag;
    entry.last_modified = last_modified;
    entry.must_revalidate = stored_directives.must_revalidate;

    const std::scoped_lock guard{mutex_};
    entry.used = ++clock_;
    if (const auto index = find_index(method, url, request_headers); index.has_value()) {
        entries_[index.value()] = std::move(entry);  // 同键的旧变体被这次的结果取代
    } else {
        entries_.push_back(std::move(entry));
    }
    evict_over_capacity();
    return true;
}

auto HttpResponseCache::revalidated(aurora::net::http::Method method, const std::string &url,
                                    const aurora::Headers &request_headers,
                                    const aurora::net::http::Response &not_modified, CacheTime now)
    -> std::optional<aurora::net::http::Response> {
    const std::scoped_lock guard{mutex_};
    const auto index = find_index(method, url, request_headers);
    if (!index.has_value()) {
        return std::nullopt;
    }
    auto entry = entries_[index.value()];
    // 头部以 304 为准并回已存的其余字段，体与状态码仍取已存那份（304 按定义没有体）。
    auto merged = merge_revalidated_headers(entry.response.headers(), not_modified.headers());
    auto combined = aurora::net::http::Response{entry.response.status(), std::string{entry.response.reason()},
                                                std::move(merged), entry.response.body(), false};
    // 新鲜度按合并后的头部现算：304 没带 `Cache-Control` 时沿用已存那枚，带了就以它为准。
    entry.freshness = freshness_of(combined, now);
    entry.etag = combined.header("ETag");
    entry.last_modified = combined.header("Last-Modified");
    entry.must_revalidate = parse_cache_control(joined_field(combined.headers(), "Cache-Control")).must_revalidate;
    entry.response = std::move(combined);
    entry.used = ++clock_;
    entries_[index.value()] = std::move(entry);
    return cached_copy(entries_[index.value()]);
}

auto HttpResponseCache::size() const -> std::size_t {
    const std::scoped_lock guard{mutex_};
    return entries_.size();
}

auto HttpResponseCache::clear() -> void {
    const std::scoped_lock guard{mutex_};
    entries_.clear();
}

auto HttpResponseCache::find_index(aurora::net::http::Method method, const std::string &url,
                                   const aurora::Headers &request_headers) const -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto &entry = entries_[i];
        if (entry.method != method || entry.url != url) {
            continue;
        }
        bool matches = true;
        for (const auto &[name, value] : entry.vary) {
            if (request_headers.get(name).value_or(std::string{}) != value) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return i;
        }
    }
    return std::nullopt;
}

auto HttpResponseCache::evict_over_capacity() -> void {
    // 表只在持锁路径上被调用，故本函数不再自行加锁。
    const auto limit = static_cast<std::size_t>(options_.max_entries);
    while (entries_.size() > limit) {
        const auto oldest = std::ranges::min_element(
            entries_, [](const CacheEntry &left, const CacheEntry &right) { return left.used < right.used; });
        entries_.erase(oldest);
    }
}

}  // namespace aurora::net::detail
