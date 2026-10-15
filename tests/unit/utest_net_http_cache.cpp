/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_cache.h + src/aurora/net/detail/http_cache.cpp
/// 测试说明: 内存缓存的判定与表的逐值回归（CHG-010 §12「条件请求与缓存校验」的算式侧）。这套判据刻意从
///           发起侧的协程体里拆出来，零 socket、判定时刻全部由参数注入，故三腿同值、wasm 腿同样可跑：
///           `Cache-Control` 文法只收本库认得的那四枚（指令名大小写不敏感、OWS 与引号内的逗号都不影响
///           切分、同份两枚 `max-age` 取后写的、不可解析的值当「没给」而不是报错、`s-maxage` 与其余外来
///           指令静默忽略——那是共享缓存的口径，本库的表是个人的）；可缓存的方法取 `GET` / `HEAD` 两枚
///           （`PUT` / `DELETE` 按重试白名单算幂等，但它们的应答描述的是「这次改动的结果」，缓存后效等于
///           把一次写留在表里）；可缓存状态码取死集合，`304` 永不在内（它是更新已有条目的输入而不是新
///           条目）；`Vary: *` 与「没给 Vary」用 `nullopt` 与空列表分家，故前者既不进表也永不命中。新鲜度
///           只认显式有效期（`max-age` 优先于 `Expires`，`Expires` 早于基准时刻折成零寿命而不是负数），
///           初始年龄取「抵达减 Date」与 `Age` 的大者（请求滞留本库不测，按 0 计并在文档写明）；响应侧
///           `no-cache` 折成「即刻过期」，与 `must-revalidate` 一样落进「过期一律先验证」这条缺省，故本库
///           不为它另开分支。表的容量按条数、逐出按最近使用（字节的闸另有发起侧两枚上限旋钮，两件事不共用
///           一枚口径），改小上限当场逐出而不是留着等命中。304 合并取 `RFC 9111 §4.3.4`：同名字段以 304
///           为准、其余保持已存的书写次序，而 `Content-Length` / `Content-Range` / `Trailer` /
///           `Transfer-Encoding` 四枚除外——它们描述的是载荷本体，而载荷本体来自已存那份。
/// 全用例零真实远端、零 socket。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/net/detail/http_cache.h"
#include "aurora/net/detail/http_retry.h"
#include "aurora/net/http.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_cache {
namespace {

using aurora::net::detail::CacheEntry;
using aurora::net::detail::CacheTime;
using aurora::net::detail::Freshness;
using aurora::net::detail::freshness_of;
using aurora::net::detail::HttpResponseCache;
using aurora::net::detail::is_cacheable_method;
using aurora::net::detail::is_cacheable_status;
using aurora::net::detail::is_fresh;
using aurora::net::detail::joined_field;
using aurora::net::detail::merge_revalidated_headers;
using aurora::net::detail::parse_cache_control;
using aurora::net::detail::parse_vary;
using aurora::net::http::CacheOptions;
using aurora::net::http::Method;
using aurora::net::http::Response;
using aurora::testing::require_value;

/// @brief 字段序列的简写：构造一份应答或请求字段表时按 (名, 值) 逐项给。
using Fields = std::vector<std::pair<std::string, std::string>>;

// `RFC 9110 §5.6.7` 的规范样例时刻：1994-11-06 08:49:37 GMT = epoch 秒 784111777。日期一律写成 IMF-fixdate
// 再折算成时刻，比的是同一套纯日历算术，故用例不依赖本机时区、也不依赖 `<ctime>` 的平台分支。
constexpr std::string_view AURORA_FIXDATE = "Sun, 06 Nov 1994 08:49:37 GMT";
constexpr std::string_view AURORA_LATER = "Sun, 06 Nov 1994 09:49:37 GMT";       // 基准 + 1 h
constexpr std::string_view AURORA_EARLIER = "Sun, 06 Nov 1994 08:44:37 GMT";     // 基准 - 5 min
constexpr std::string_view AURORA_NEXT_DAY = "Mon, 07 Nov 1994 08:49:37 GMT";    // 基准 + 1 d

/// @brief 把 IMF-fixdate 折成判定基准时刻。
/// @param fixdate 日期文本。
/// @return 对应时刻（文法不合即让用例当场失败，不在断言里静默退化成「没给」）。
[[nodiscard]] auto at(std::string_view fixdate) -> CacheTime {
    return require_value(aurora::net::detail::parse_http_date(fixdate));
}

/// @brief 秒的字面量写法（本仓的测试与实现一律写全称，不开 `chrono_literals` 这第二个口径）。
/// @param value 秒数。
/// @return 对应时长。
[[nodiscard]] constexpr auto secs(std::int64_t value) -> std::chrono::seconds {
    return std::chrono::seconds{value};
}

/// @brief 把时刻折成「自 epoch 起的秒」，使时间点的相等性比的是一个可打印的整数。
/// @param when 待折算的时刻。
/// @return 秒计数。
[[nodiscard]] auto epoch_seconds(CacheTime when) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
}

/// @brief 造一份应答：状态码 + 字段表 + 体文本。同名字段重复给即多行字段。
/// @param status 三位状态码。
/// @param fields 字段序列。
/// @param body 消息体文本。
/// @return 可直接喂给判定函数与表的应答。
[[nodiscard]] auto replied(std::uint16_t status, const Fields &fields, std::string_view body = {}) -> Response {
    aurora::Headers headers;
    for (const auto &[name, value] : fields) {
        headers.append(name, value);
    }
    std::vector<char> payload{body.begin(), body.end()};
    return Response{status, "OK", std::move(headers), std::move(payload), false};
}

/// @brief 造一份请求字段表（`Vary` 匹配与请求侧 `Cache-Control` 都读它）。
/// @param fields 字段序列。
/// @return 字段表。
[[nodiscard]] auto requested(const Fields &fields) -> aurora::Headers {
    aurora::Headers headers;
    for (const auto &[name, value] : fields) {
        headers.append(name, value);
    }
    return headers;
}

/// @brief 把消息体读成文本，便于按内容断言「交出的是表里那一份」。
/// @param response 应答。
/// @return 体文本副本。
[[nodiscard]] auto as_text(const Response &response) -> std::string {
    return std::string{response.body().begin(), response.body().end()};
}

/// @brief 取一枚应答的字段值（缺失折成空串，使「没给」与「给了空值」在本用例集里不必分家）。
/// @param response 应答。
/// @param name 字段名。
/// @return 值文本。
[[nodiscard]] auto field_of(const Response &response, std::string_view name) -> std::string {
    return response.header(name).value_or(std::string{});
}

/// @brief 无 `Vary` 时表用的键：一个足够稳定的目标 URL。
///
/// 取函数形态而不是文件级常量：`HttpResponseCache` 的入口收 `const std::string &`，而 `bugprone-throwing-
/// static-initialization` 不许命名空间作用域上放一枚可能抛出的 `std::string`（静态初始化期抛出的异常无人接）。
/// @return 建键用的 URL 文本。
[[nodiscard]] auto cache_url() -> std::string {
    return std::string{"http://127.0.0.1/cache"};
}

/// @brief 一份必可进表的新鲜应答：`max-age` 600 秒、`Date` 与抵达时刻同值（初始年龄 0）。
/// @param body 体文本。
/// @return 应答。
[[nodiscard]] auto fresh(std::string_view body = "ok") -> Response {
    return replied(std::uint16_t{200},
                   Fields{{"Cache-Control", "max-age=600"}, {"Date", std::string{AURORA_FIXDATE}}}, body);
}

/// @brief 造一张开着的表。
/// @param max_entries 容量（条数）。
/// @return 表（用例栈上持有）。
[[nodiscard]] auto table(std::uint32_t max_entries) -> HttpResponseCache {
    return HttpResponseCache{CacheOptions{.enabled = true, .max_entries = max_entries}};
}

/// @brief 把 `Vary` 匹配用的请求头按名字读回字段表，省掉每条用例重贴一段模板。
/// @param name 字段名。
/// @param value 字段值。
/// @return 单字段请求表。
[[nodiscard]] auto one_request(std::string_view name, std::string_view value) -> aurora::Headers {
    return requested(Fields{{std::string{name}, std::string{value}}});
}

}  // namespace

AURORA_TEST_CASE(cache_directive_grammar_reads_only_the_honoured_set) {
    const auto plain = parse_cache_control("max-age=60, must-revalidate");
    AURORA_TEST_CHECK_FALSE(plain.no_store);
    AURORA_TEST_CHECK_FALSE(plain.no_cache);
    AURORA_TEST_CHECK(plain.must_revalidate);
    AURORA_TEST_REQUIRE(plain.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(plain.max_age).count(), std::int64_t{60});

    // 指令名大小写不敏感、值前后的 OWS 都吃掉：字段值是 token 而不是字面量比对。
    const auto untidy = parse_cache_control("  No-Cache , MAX-AGE =  7 ");
    AURORA_TEST_CHECK(untidy.no_cache);
    AURORA_TEST_REQUIRE(untidy.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(untidy.max_age).count(), std::int64_t{7});

    // 引号内的逗号不是分隔符（`token="a,b"` 是合法写法），引号本身也不留在值里参与解析。
    const auto quoted = parse_cache_control("ext=\"a,b\", max-age=3, no-store");
    AURORA_TEST_REQUIRE(quoted.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(quoted.max_age).count(), std::int64_t{3});
    AURORA_TEST_CHECK(quoted.no_store);
    AURORA_TEST_CHECK_FALSE(quoted.no_cache);

    // 同份里出现两枚 `max-age`：后写覆盖先写，与字段的书写次序一致。
    const auto twice = parse_cache_control("max-age=100, max-age=5");
    AURORA_TEST_REQUIRE(twice.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(twice.max_age).count(), std::int64_t{5});

    // 值不可解析按「没给」处理而不是报错：本库不该因为对端多打了一个空格就拒绝缓存一条能缓存的应答。
    const auto broken = parse_cache_control("max-age=abc, max-age=-5, max-age=");
    AURORA_TEST_CHECK_FALSE(broken.max_age.has_value());
    // 天文数字不绕成负数也不当「没给」：纯数字只是超出可表示范围即饱和，意思就是「很久不用问」。
    const auto huge = parse_cache_control("max-age=99999999999999999999");
    AURORA_TEST_REQUIRE(huge.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(huge.max_age).count(), std::numeric_limits<std::int64_t>::max());
    // `uint64` 之内、`int64` 之外的值同样饱和，不会绕成负寿命（负寿命等于「永远不交付」）。
    const auto wide = parse_cache_control("max-age=9223372036854775808");
    AURORA_TEST_REQUIRE(wide.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(wide.max_age).count(), std::numeric_limits<std::int64_t>::max());

    // 外来指令一律忽略且不报错——`Cache-Control` 是可扩展字段。`s-maxage` 也忽略：那是给共享缓存的，
    // 本库的表是个人的，按 `RFC 9111 §5.2.2.9` 本就不该听它。
    const auto foreign = parse_cache_control("private, public, immutable, s-maxage=10, max-stale=30");
    AURORA_TEST_CHECK_FALSE(foreign.no_store);
    AURORA_TEST_CHECK_FALSE(foreign.no_cache);
    AURORA_TEST_CHECK_FALSE(foreign.must_revalidate);
    AURORA_TEST_CHECK_FALSE(foreign.max_age.has_value());
}

AURORA_TEST_CASE(only_get_and_head_may_be_stored) {
    AURORA_TEST_CHECK(is_cacheable_method(Method::Get));
    AURORA_TEST_CHECK(is_cacheable_method(Method::Head));
    // 与重试白名单刻意不同：那一份按幂等取，这一份还要「应答不描述一次改动的结果」。
    AURORA_TEST_CHECK_FALSE(is_cacheable_method(Method::Post));
    AURORA_TEST_CHECK_FALSE(is_cacheable_method(Method::Put));
    AURORA_TEST_CHECK_FALSE(is_cacheable_method(Method::Patch));
    AURORA_TEST_CHECK_FALSE(is_cacheable_method(Method::Delete));
}

AURORA_TEST_CASE(the_cacheable_status_family_is_a_dead_set) {
    for (const std::uint16_t status : {std::uint16_t{200}, std::uint16_t{203}, std::uint16_t{300},
                                       std::uint16_t{301}, std::uint16_t{404}, std::uint16_t{405},
                                       std::uint16_t{410}, std::uint16_t{414}, std::uint16_t{501}}) {
        AURORA_TEST_CHECK(is_cacheable_status(status));
    }
    // `5xx` 里除 501 都不进：服务器此刻不行了，不代表这个资源长这样。
    for (const std::uint16_t status : {std::uint16_t{201}, std::uint16_t{204}, std::uint16_t{302},
                                       std::uint16_t{400}, std::uint16_t{401}, std::uint16_t{429},
                                       std::uint16_t{500}, std::uint16_t{502}, std::uint16_t{503}}) {
        AURORA_TEST_CHECK_FALSE(is_cacheable_status(status));
    }
    // 304 是更新已有条目的输入，不是一条新条目。
    AURORA_TEST_CHECK_FALSE(is_cacheable_status(std::uint16_t{304}));
}

AURORA_TEST_CASE(a_wildcard_vary_is_not_the_same_as_no_vary) {
    const auto named = require_value(parse_vary("Accept-Encoding, Accept-Language"));
    AURORA_TEST_CHECK_EQ(named.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(named.at(0), std::string{"accept-encoding"});  // 名字按 ASCII 小写归一
    AURORA_TEST_CHECK_EQ(named.at(1), std::string{"accept-language"});

    // 重复列出的同一枚头只记一次：`Vary` 的语义是集合，写两遍不改变匹配面。
    const auto listed_twice = require_value(parse_vary("Accept, accept, ACCEPT"));
    AURORA_TEST_CHECK_EQ(listed_twice.size(), std::size_t{1});

    // `*` 意为「随任何请求特征而变」，与「没给 Vary」是两回事，故用 nullopt 而不是空列表表达。
    AURORA_TEST_CHECK_FALSE(parse_vary("*").has_value());
    AURORA_TEST_CHECK_FALSE(parse_vary("Accept, *").has_value());  // 混着给也按通配处理
    const auto none = require_value(parse_vary(""));
    AURORA_TEST_CHECK_EQ(none.size(), std::size_t{0});
}

AURORA_TEST_CASE(repeated_lines_join_before_anything_is_parsed) {
    // `Cache-Control` 与 `Vary` 都允许分行给；只取第一行会把「`no-store` 写在第二行」读成普通应答。
    const aurora::Headers headers = requested(Fields{
        {"Cache-Control", "max-age=30"}, {"cache-control", "no-store"}, {"Vary", "Accept"}, {"Vary", "Age"}});
    const auto directives = parse_cache_control(joined_field(headers, "Cache-Control"));
    AURORA_TEST_CHECK(directives.no_store);
    AURORA_TEST_REQUIRE(directives.max_age.has_value());
    AURORA_TEST_CHECK_EQ(require_value(directives.max_age).count(), std::int64_t{30});

    const auto vary = require_value(parse_vary(joined_field(headers, "Vary")));
    AURORA_TEST_CHECK_EQ(vary.size(), std::size_t{2});

    // 字段缺失即空串：解析器把它读成「一条指令都没有」，而不是报错。
    AURORA_TEST_CHECK_EQ(joined_field(headers, "If-None-Match").size(), std::size_t{0});
    const auto absent = parse_cache_control(joined_field(headers, "Cache-X"));
    AURORA_TEST_CHECK_FALSE(absent.no_store);
    AURORA_TEST_CHECK_FALSE(absent.max_age.has_value());
}

AURORA_TEST_CASE(freshness_prefers_max_age_over_expires) {
    const auto arrived = at(AURORA_FIXDATE);

    const auto by_max_age = replied(std::uint16_t{200},
                                    Fields{{"Cache-Control", "max-age=60"},
                                           {"Expires", std::string{AURORA_NEXT_DAY}}},
                                    "ok");
    const auto max_age = freshness_of(by_max_age, arrived);
    AURORA_TEST_CHECK_EQ(max_age.lifetime, secs(60));  // `max-age` 说话时 `Expires` 让位
    AURORA_TEST_CHECK_EQ(max_age.initial_age, std::int64_t{0});  // 没有 `Date` 也没有 `Age`：无从得知它旧
    AURORA_TEST_CHECK_EQ(epoch_seconds(max_age.arrived), epoch_seconds(arrived));

    // 只有 `Expires` 时按它减基准时刻；有 `Date` 就以 `Date` 为基准（那是「何时起算」的答案）。
    const auto by_expires = replied(std::uint16_t{200},
                                    Fields{{"Date", std::string{AURORA_FIXDATE}},
                                           {"Expires", std::string{AURORA_LATER}}},
                                    "ok");
    AURORA_TEST_CHECK_EQ(freshness_of(by_expires, arrived).lifetime, secs(3600));

    // 过期的 `Expires` 折成零寿命而不是负数：即刻过期意味着「必须先问对端」，不意味着「永远别交付」。
    const auto expired_expires = replied(std::uint16_t{200},
                                         Fields{{"Date", std::string{AURORA_FIXDATE}},
                                                {"Expires", std::string{AURORA_EARLIER}}},
                                         "ok");
    AURORA_TEST_CHECK_EQ(freshness_of(expired_expires, arrived).lifetime, secs(0));

    // 没有 `Date` 时以抵达时刻为基准（`RFC 9111 §4.2.1` 的「已知基准就用基准」）。
    const auto only_expires =
        replied(std::uint16_t{200}, Fields{{"Expires", std::string{AURORA_LATER}}}, "ok");
    AURORA_TEST_CHECK_EQ(freshness_of(only_expires, arrived).lifetime, secs(3600));

    // 日期文法不合即当「没给」：一条畸形 `Date` 不该把一份本来能缓存的应答变成永远不新鲜。
    const auto junk_date = replied(std::uint16_t{200},
                                   Fields{{"Cache-Control", "max-age=5"}, {"Date", "not an http date"}}, "ok");
    AURORA_TEST_CHECK_EQ(freshness_of(junk_date, arrived).lifetime, secs(5));
    AURORA_TEST_CHECK_EQ(freshness_of(junk_date, arrived).initial_age, std::int64_t{0});

    // 两者皆无即零寿命：调用方须另判有无校验子（本库不做启发式新鲜度）。
    const auto no_validity =
        replied(std::uint16_t{200}, Fields{{"Date", std::string{AURORA_FIXDATE}}}, "ok");
    AURORA_TEST_CHECK_EQ(freshness_of(no_validity, arrived).lifetime, secs(0));
}

AURORA_TEST_CASE(initial_age_takes_the_larger_of_the_date_gap_and_age) {
    const auto arrived = at(AURORA_FIXDATE);
    auto age_of = [&arrived](const Fields &fields) {
        return freshness_of(replied(std::uint16_t{200}, fields), arrived).initial_age;
    };

    // `Date` 比抵达早 300 秒 → apparent_age 300；`Age: 42` 更小，故取大者。
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Date", std::string{AURORA_EARLIER}}, {"Age", "42"}}), std::int64_t{300});
    // 中间代理说的更旧就用代理的：`Age` 是它那一侧的累计量，比本库从 `Date` 反推的更贴近事实。
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Date", std::string{AURORA_EARLIER}}, {"Age", "400"}}), std::int64_t{400});
    // `Date` 在未来（对端或本机时钟漂了）不制造负年龄。
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Date", std::string{AURORA_LATER}}}), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Age", "0"}}), std::int64_t{0});
    // `Age` 畸形（带符号、带小数、混字母）一律当「没给」，不影响判定本身。
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Age", "-5"}}), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Age", "1.5"}}), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(age_of(Fields{{"Age", "abc"}}), std::int64_t{0});
    // 请求滞留本库不测（按 0 计并在文档写明），故 `max-age` 与 `Age` 相同时抵达即过期。
    const auto shared = freshness_of(
        replied(std::uint16_t{200}, Fields{{"Age", "60"}, {"Cache-Control", "max-age=60"}}, "ok"), arrived);
    AURORA_TEST_CHECK_EQ(shared.initial_age, std::int64_t{60});
    AURORA_TEST_CHECK_FALSE(is_fresh(CacheEntry{.freshness = shared}, arrived));

    // 天文数字的 `Age` 饱和到上界，而判定先把加法夹住再比：老掉牙的条目不能因为绕回负数读成「刚刚抵达」。
    const auto ancient = freshness_of(
        replied(std::uint16_t{200}, Fields{{"Age", "99999999999999999999"}, {"Cache-Control", "max-age=60"}}, "ok"),
        arrived);
    AURORA_TEST_CHECK_EQ(ancient.initial_age, std::numeric_limits<std::int64_t>::max());
    AURORA_TEST_CHECK_FALSE(is_fresh(CacheEntry{.freshness = ancient}, arrived + secs(30)));
}

AURORA_TEST_CASE(a_response_marked_no_cache_is_stored_but_never_delivered_without_asking) {
    const auto arrived = at(AURORA_FIXDATE);
    const auto always_ask = replied(std::uint16_t{200},
                                    Fields{{"Cache-Control", "no-cache, max-age=600"},
                                           {"Date", std::string{AURORA_FIXDATE}}, {"ETag", "\"v1\""}},
                                    "ok");
    // 响应侧 `no-cache` 折成「即刻过期」：`max-age=600` 让位，因为那条指令说的就是「用之前先核对」。
    AURORA_TEST_CHECK_EQ(freshness_of(always_ask, arrived).lifetime, secs(0));

    auto cache = table(8);
    AURORA_TEST_REQUIRE(cache.store(Method::Get, cache_url(), aurora::Headers{}, always_ask, arrived));
    const auto hit = cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived);
    AURORA_TEST_CHECK_FALSE(hit.fresh.has_value());
    const auto pending = require_value(hit.revalidate);
    AURORA_TEST_CHECK_EQ(require_value(pending.etag), std::string{"\"v1\""});
    AURORA_TEST_CHECK_FALSE(pending.last_modified.has_value());

    // `must-revalidate` 不需要额外分支：本库缺省就是「过期一律验证」，它的禁令正是缺省本身。
    const auto directives = parse_cache_control("max-age=60, must-revalidate");
    AURORA_TEST_CHECK(directives.must_revalidate);
    auto revalidated_cache = table(8);
    AURORA_TEST_REQUIRE(revalidated_cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Cache-Control", "max-age=60, must-revalidate"},
                                           {"Date", std::string{AURORA_FIXDATE}}},
                "ok"),
        arrived));
    AURORA_TEST_CHECK(
        revalidated_cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived + secs(30)).fresh.has_value());
    AURORA_TEST_CHECK_FALSE(
        revalidated_cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived + secs(60)).fresh.has_value());
}

AURORA_TEST_CASE(residence_ages_an_entry_and_a_backwards_clock_does_not) {
    const auto arrived = at(AURORA_FIXDATE);
    CacheEntry entry;
    entry.freshness = Freshness{.lifetime = secs(60), .initial_age = 10, .arrived = arrived};
    AURORA_TEST_CHECK(is_fresh(entry, arrived));  // 年龄 10 < 60
    AURORA_TEST_CHECK(is_fresh(entry, arrived + secs(49)));  // 59 < 60
    AURORA_TEST_CHECK_FALSE(is_fresh(entry, arrived + secs(50)));  // 边界：60 不小于 60，过期不是「差不多过期」
    AURORA_TEST_CHECK_FALSE(is_fresh(entry, arrived + secs(3600)));
    // 本机时钟往回走过（或 `Date` 在未来）不制造负年龄：居住时长下限就是 0。
    AURORA_TEST_CHECK(is_fresh(entry, arrived - secs(120)));

    CacheEntry born_expired;
    born_expired.freshness = Freshness{.lifetime = secs(0), .initial_age = 0, .arrived = arrived};
    AURORA_TEST_CHECK_FALSE(is_fresh(born_expired, arrived));
}

AURORA_TEST_CASE(a_fresh_entry_is_delivered_and_says_it_came_from_the_table) {
    const auto arrived = at(AURORA_FIXDATE);
    auto cache = table(8);
    AURORA_TEST_REQUIRE(cache.store(Method::Get, cache_url(), aurora::Headers{}, fresh("body-1"), arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{1});

    const auto hit = require_value(cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived + secs(30)).fresh);
    AURORA_TEST_CHECK(hit.from_cache());  // 交付侧的标记：调用方据此知道本次没触网
    AURORA_TEST_CHECK_EQ(hit.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(as_text(hit), std::string{"body-1"});

    // 600 秒到点即不再新鲜，而这条没有校验子可挂 → 两个结果都空（发起侧据此走一次普通请求）。
    const auto stale = cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived + secs(600));
    AURORA_TEST_CHECK_FALSE(stale.fresh.has_value());
    AURORA_TEST_CHECK_FALSE(stale.revalidate.has_value());

    // 键是 (方法, URL)：换目标、换方法都不命中。
    AURORA_TEST_CHECK_FALSE(
        cache.lookup(Method::Get, "http://127.0.0.1/other", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_CHECK_FALSE(cache.lookup(Method::Head, cache_url(), aurora::Headers{}, arrived).fresh.has_value());

    // 表里存的是「未标缓存」的形态：标记只在交付那一刻置真，否则条目自己会被读成一次命中。
    auto validator_cache = table(8);
    AURORA_TEST_REQUIRE(validator_cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Date", std::string{AURORA_FIXDATE}}, {"ETag", "\"v1\""}}, "ok"),
        arrived));
    AURORA_TEST_CHECK_FALSE(
        require_value(validator_cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived).revalidate)
            .response.from_cache());
}

AURORA_TEST_CASE(vary_values_decide_whether_the_second_request_hits) {
    const auto arrived = at(AURORA_FIXDATE);
    const auto varies = replied(std::uint16_t{200},
                               Fields{{"Cache-Control", "max-age=600"}, {"Vary", "Accept-Language"},
                                      {"Date", std::string{AURORA_FIXDATE}}},
                               "zh");
    auto cache = table(8);
    AURORA_TEST_REQUIRE(cache.store(Method::Get, cache_url(), one_request("Accept-Language", "zh"), varies, arrived));

    // 同名同值才命中：`Vary` 存的是「应答侧点名的头名 + 当时请求里那枚头的取值」。
    AURORA_TEST_CHECK(cache.lookup(Method::Get, cache_url(), one_request("Accept-Language", "zh"), arrived).fresh.has_value());
    AURORA_TEST_CHECK_FALSE(
        cache.lookup(Method::Get, cache_url(), one_request("Accept-Language", "en"), arrived).fresh.has_value());
    // 第二次请求干脆没带那枚头 = 另一种请求，不配复用为中文那份。
    AURORA_TEST_CHECK_FALSE(cache.lookup(Method::Get, cache_url(), aurora::Headers{}, arrived).fresh.has_value());

    // 同一 URL 可以并存多份变体：写入 `en` 那份不挤掉 `zh` 那份。
    const auto english = replied(std::uint16_t{200},
                                 Fields{{"Cache-Control", "max-age=600"}, {"Vary", "Accept-Language"},
                                        {"Date", std::string{AURORA_FIXDATE}}},
                                 "en");
    AURORA_TEST_REQUIRE(cache.store(Method::Get, cache_url(), one_request("Accept-Language", "en"), english, arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(as_text(require_value(
                             cache.lookup(Method::Get, cache_url(), one_request("Accept-Language", "zh"), arrived).fresh)),
                         std::string{"zh"});
    AURORA_TEST_CHECK_EQ(as_text(require_value(
                             cache.lookup(Method::Get, cache_url(), one_request("Accept-Language", "en"), arrived).fresh)),
                         std::string{"en"});

    // 同键的第二次写入取代旧变体（同一请求特征不该在表里留两条说法）。
    AURORA_TEST_REQUIRE(
        cache.store(Method::Get, cache_url(), one_request("Accept-Language", "en"), fresh("en-2"), arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(as_text(require_value(
                             cache.lookup(Method::Get, cache_url(), one_request("Accept-Language", "en"), arrived).fresh)),
                         std::string{"en-2"});
}

AURORA_TEST_CASE(five_gates_keep_a_response_out_of_the_table) {
    const auto arrived = at(AURORA_FIXDATE);
    auto cache = table(8);

    // ① 方法：写操作的应答描述的是「这次改动的结果」，不缓存后效。
    AURORA_TEST_CHECK_FALSE(cache.store(Method::Post, cache_url(), aurora::Headers{}, fresh(), arrived));
    // ② 状态码：不在那一族里即不进表。
    AURORA_TEST_CHECK_FALSE(
        cache.store(Method::Get, cache_url(), aurora::Headers{}, replied(std::uint16_t{500}, Fields{}, "no"), arrived));
    // ③ 响应侧 `no-store`：连读都不该读，强制 bypass。
    AURORA_TEST_CHECK_FALSE(cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Cache-Control", "no-store, max-age=60"},
                                           {"Date", std::string{AURORA_FIXDATE}}},
                "secret"),
        arrived));
    // ④ 请求侧 `no-store`：调用方在请求里就说了别存。
    AURORA_TEST_CHECK_FALSE(cache.store(Method::Get, cache_url(), one_request("Cache-Control", "no-store"), fresh(),
                                        arrived));
    // ⑤ `Vary: *`：随任何请求特征而变的应答不可复用。
    AURORA_TEST_CHECK_FALSE(cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Cache-Control", "max-age=60"}, {"Vary", "*"},
                                           {"Date", std::string{AURORA_FIXDATE}}},
                "ok"),
        arrived));
    // ⑥ 既没有效期又没有任何校验子：存了只能靠启发式交付，而本库不做启发式新鲜度。
    AURORA_TEST_CHECK_FALSE(
        cache.store(Method::Get, cache_url(), aurora::Headers{}, replied(std::uint16_t{200}, Fields{}, "ok"), arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{0});

    // 只有校验子而无有效期的照进表，但即刻过期：省的是体字节，不省那一次往返。
    AURORA_TEST_REQUIRE(cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Date", std::string{AURORA_FIXDATE}}, {"ETag", "\"v1\""}}, "ok"),
        arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{1});
}

AURORA_TEST_CASE(the_table_evicts_the_least_recently_used_entry) {
    const auto arrived = at(AURORA_FIXDATE);
    auto cache = table(2);
    AURORA_TEST_REQUIRE(cache.store(Method::Get, "http://127.0.0.1/a", aurora::Headers{}, fresh("a"), arrived));
    AURORA_TEST_REQUIRE(cache.store(Method::Get, "http://127.0.0.1/b", aurora::Headers{}, fresh("b"), arrived));

    // 读一次就是「用过一次」：a 因此比 b 更近，逐出该轮到 b。
    AURORA_TEST_CHECK(cache.lookup(Method::Get, "http://127.0.0.1/a", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_REQUIRE(cache.store(Method::Get, "http://127.0.0.1/c", aurora::Headers{}, fresh("c"), arrived));
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{2});
    AURORA_TEST_CHECK_FALSE(
        cache.lookup(Method::Get, "http://127.0.0.1/b", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_CHECK_EQ(as_text(require_value(
                             cache.lookup(Method::Get, "http://127.0.0.1/a", aurora::Headers{}, arrived).fresh)),
                         std::string{"a"});
    AURORA_TEST_CHECK_EQ(as_text(require_value(
                             cache.lookup(Method::Get, "http://127.0.0.1/c", aurora::Headers{}, arrived).fresh)),
                         std::string{"c"});

    // 容量为 0 与「不存任何东西」同义：当场逐出，而不是留着等命中。
    auto nothing = table(0);
    AURORA_TEST_CHECK(nothing.store(Method::Get, cache_url(), aurora::Headers{}, fresh(), arrived));
    AURORA_TEST_CHECK_EQ(nothing.size(), std::size_t{0});
}

AURORA_TEST_CASE(shrinking_the_capacity_evicts_down_to_the_new_limit) {
    const auto arrived = at(AURORA_FIXDATE);
    auto cache = table(4);
    for (const std::string_view letter : {std::string_view{"a"}, std::string_view{"b"}, std::string_view{"c"},
                                          std::string_view{"d"}}) {
        AURORA_TEST_REQUIRE(cache.store(
            Method::Get, std::string{"http://127.0.0.1/"} + std::string{letter}, aurora::Headers{}, fresh(letter),
            arrived));
    }
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{4});

    cache.set_options(CacheOptions{.enabled = true, .max_entries = 2});
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{2});
    // 留下的是最近使用的两枚（c、d）：逐出依据是使用序号，不是写入次序的巧合。
    AURORA_TEST_CHECK_FALSE(
        cache.lookup(Method::Get, "http://127.0.0.1/a", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_CHECK_FALSE(
        cache.lookup(Method::Get, "http://127.0.0.1/b", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_CHECK(cache.lookup(Method::Get, "http://127.0.0.1/c", aurora::Headers{}, arrived).fresh.has_value());
    AURORA_TEST_CHECK(cache.lookup(Method::Get, "http://127.0.0.1/d", aurora::Headers{}, arrived).fresh.has_value());

    // 开关关掉不清表：`enabled` 由发起侧判，本类不读它（要清表请显式 `clear()`）。
    cache.set_options(CacheOptions{.enabled = false, .max_entries = 2});
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{2});
    cache.clear();
    AURORA_TEST_CHECK_EQ(cache.size(), std::size_t{0});
}

AURORA_TEST_CASE(a_revalidated_entry_keeps_the_body_and_adopts_the_new_headers) {
    const auto arrived = at(AURORA_FIXDATE);
    auto cache = table(8);
    AURORA_TEST_REQUIRE(cache.store(
        Method::Get, cache_url(), aurora::Headers{},
        replied(std::uint16_t{200}, Fields{{"Cache-Control", "max-age=60"}, {"Date", std::string{AURORA_FIXDATE}},
                                           {"ETag", "\"v1\""}, {"Content-Length", "2"}, {"X-Old", "1"}},
                "ok"),
        arrived));

    const auto now = at(AURORA_LATER);
    const auto not_modified = replied(std::uint16_t{304},
                                      Fields{{"Date", std::string{AURORA_LATER}}, {"Cache-Control", "max-age=120"},
                                             {"ETag", "\"v2\""}, {"X-New", "9"}, {"Content-Length", "0"}},
                                      "");
    const auto merged = require_value(cache.revalidated(Method::Get, cache_url(), aurora::Headers{}, not_modified, now));
    // 状态码与体仍取已存那份（304 按定义没有体），头部以 304 为准并回其余字段。
    AURORA_TEST_CHECK_EQ(merged.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(as_text(merged), std::string{"ok"});
    AURORA_TEST_CHECK(merged.from_cache());
    AURORA_TEST_CHECK_EQ(field_of(merged, "X-Old"), std::string{"1"});
    AURORA_TEST_CHECK_EQ(field_of(merged, "X-New"), std::string{"9"});
    AURORA_TEST_CHECK_EQ(field_of(merged, "ETag"), std::string{"\"v2\""});  // 校验子跟着新头部走
    AURORA_TEST_CHECK_EQ(field_of(merged, "Content-Length"), std::string{"2"});  // 304 的那枚 0 不许覆盖

    // 新鲜度按合并后的头部现算：304 给了 `max-age=120` 就以它为准，从 304 的抵达时刻起算。
    AURORA_TEST_CHECK(cache.lookup(Method::Get, cache_url(), aurora::Headers{}, now + secs(119)).fresh.has_value());
    const auto again = cache.lookup(Method::Get, cache_url(), aurora::Headers{}, now + secs(120));
    AURORA_TEST_CHECK_FALSE(again.fresh.has_value());
    AURORA_TEST_CHECK_EQ(require_value(require_value(again.revalidate).etag), std::string{"\"v2\""});

    // 表里没有对应条目（被逐出、或压根没存过）时无从合并：交回空值，发起侧据此交出那份 304。
    AURORA_TEST_CHECK_FALSE(
        cache.revalidated(Method::Get, "http://127.0.0.1/never", aurora::Headers{}, not_modified, now).has_value());
    auto empty = table(4);
    AURORA_TEST_CHECK_FALSE(
        empty.revalidated(Method::Get, cache_url(), aurora::Headers{}, not_modified, now).has_value());
}

AURORA_TEST_CASE(merging_skips_the_four_fields_that_describe_the_payload) {
    const aurora::Headers stored = requested(Fields{{"Content-Length", "12"},
                                                     {"Content-Range", "bytes 0-11/12"},
                                                     {"Trailer", "X-Stuff"},
                                                     {"Transfer-Encoding", "gzip"},
                                                     {"ETag", "\"v1\""},
                                                     {"X-Keep", "a"}});
    const aurora::Headers update = requested(Fields{{"Content-Length", "0"},
                                                    {"Content-Range", "bytes 9-9/9"},
                                                    {"Trailer", "Nope"},
                                                    {"Transfer-Encoding", "identity"},
                                                    {"ETag", "\"v2\""},
                                                    {"X-New", "b"},
                                                    {"Set-Cookie", "a=1"},
                                                    {"Set-Cookie", "b=2"}});
    const auto merged = merge_revalidated_headers(stored, update);

    // 四枚载荷描述字段保持已存的那一份：让一次没有消息体的应答回答「这条消息有多长」是错的。
    AURORA_TEST_CHECK_EQ(merged.get("Content-Length").value_or(std::string{}), std::string{"12"});
    AURORA_TEST_CHECK_EQ(merged.get("Content-Range").value_or(std::string{}), std::string{"bytes 0-11/12"});
    AURORA_TEST_CHECK_EQ(merged.get("Trailer").value_or(std::string{}), std::string{"X-Stuff"});
    AURORA_TEST_CHECK_EQ(merged.get("Transfer-Encoding").value_or(std::string{}), std::string{"gzip"});
    // 其余字段以 304 为准，304 没提的原样留着。
    AURORA_TEST_CHECK_EQ(merged.get("ETag").value_or(std::string{}), std::string{"\"v2\""});
    AURORA_TEST_CHECK_EQ(merged.get("X-Keep").value_or(std::string{}), std::string{"a"});
    AURORA_TEST_CHECK_EQ(merged.get("X-New").value_or(std::string{}), std::string{"b"});
    // 书写次序跟着已存那份走：四枚载荷描述字段仍在原位（`Headers::set` 的既有语义是「替换即移到末尾」，
    // 故被 304 改写的那枚排到没被改写的字段之后——次序稳定即可，不承诺原位）。
    AURORA_TEST_CHECK_EQ(merged.entries().at(0).name, std::string{"Content-Length"});
    AURORA_TEST_CHECK_EQ(merged.entries().at(3).name, std::string{"Transfer-Encoding"});
    AURORA_TEST_CHECK_EQ(merged.entries().at(4).name, std::string{"X-Keep"});
    AURORA_TEST_CHECK_EQ(merged.size(), std::size_t{9});
    // 同名多值不因 304 只给一枚而缩水成一条，也不因合并而重复。
    AURORA_TEST_CHECK_EQ(merged.values("Set-Cookie").size(), std::size_t{2});
    // 304 没给的新字段不凭空出现：只并它带来的那几枚。
    AURORA_TEST_CHECK_EQ(merged.values("Vary").size(), std::size_t{0});
}

}  // namespace aurora::test_cases::utest_net_http_cache
