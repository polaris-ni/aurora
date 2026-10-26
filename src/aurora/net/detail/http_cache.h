// http_cache.h —— 内存缓存的判定与表。
//
// 为何单列：一次应答「能不能存、存多久、什么时候必须问对端、304 回来之后怎么并」是 `RFC 9111` 的一套独立
// 算式，与「怎么把字节读回来」（`wire::ResponseAssembler`）、「该不该再打一遍」（`http_retry.h`）都无关。
// 写进 `Client::send` 的协程体就只能靠真对端才能测，而「`no-store` 不该进表」「`Vary: *` 恒不命中」「无
// 显式有效期又无校验子不该进表」这类判定恰恰是最不该靠现场发现的东西。落在这里，零 socket、判定基准时刻由
// 参数注入，故可逐值断言且三腿同值；发起侧只在三处各问一声：起手查表、拿到结局问进不进表、304 到手要一份
// 合并后的应答。
//
// 六条口径（规范性，落码与文档回写以此为准）：
// - **表按 (方法, 目标 URL, `Vary` 点名的请求头值) 建键**：`Vary` 存的是应答侧点名的头名与**当时请求里那些
//   头的取值**，后续请求只有在同名同值时才命中。`Vary: *` 意为「这份应答随任何请求特征而变」，既不进表也
//   永不命中（`RFC 9111 §4.4`）。发起侧还能追加变体名（钩子触点集走的就是这一条）：三处入口各收一组名字，
//   与应答 `Vary` 和凭据名去重后一并存入；**条目缺当前任何一枚追加名的即不命中**——一枚钩子这次多带了
//   `X-Tenant`，上一次存下的副本没把它算进键，照常命中就等于让新租户读到旧租户的内容。
// - **从不交付陈旧副本**：过期条目必须先向对端验证，验证失败就把那次失败原样交出，绝不拿旧数据兜场。故
//   `must-revalidate` 在本库不需要额外分支——它的禁令正是本库缺省；`no-cache` 响应指令折算为「即刻过期」。
// - **不做启发式新鲜度**（`RFC 9111 §4.2.2`）：那套算法拿 `Last-Modified` 与 `Date` 的比例猜寿命，同一个
//   应答在不同抵达时刻会得到不同寿命，判据写不成定值。没有显式有效期又没有任何校验子的应答**不进表**；
//   有校验子而无有效期的照进表但即刻过期——省的是体字节，不省那一次往返。
// - **只存幂等且不改变状态的那两枚方法**：`GET` / `HEAD`。`PUT` / `DELETE` 虽然按重试白名单算幂等，但它们
//   的应答同样描述的是「这次改动的结果」，缓存那份结果等于把一次写操作的后效留在表里。
// - **可缓存状态码取死集合**：200 / 203 / 300 / 301 / 404 / 405 / 410 / 414 / 501（`RFC 9111 §4.2.2` 列举
//   的那一族，含「肯定答案」与「明确不存在」两类）。`5xx` 里除 501 外都不进——服务器此刻不行了，不代表
//   这个资源长这样；`304` 永不进表，它是更新已有条目的输入而不是新条目。
// - **容量按条数、逐出按最近使用**：`max_entries` 管条数，字节的闸另有发起侧那两枚上限旋钮，两件事不共用
//   一枚口径。表满逐出最久未使用的那条；条目本身不设独立 TTL，过期只在命中时现算。
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {

/// @brief 缓存判定的基准时刻；与 `Retry-After` 的日期形态同源用墙上时钟（`Date` / `Expires` 本来就是它）。
using CacheTime = std::chrono::system_clock::time_point;

/// @brief `Cache-Control` 里本库认得的那几枚指令；其余（`public` / `private` / `immutable` 等）一律忽略。
///
/// 忽略不等于报错：`Cache-Control` 是可扩展字段，未来的指令对本库的意义由本库未来的版本决定。`s-maxage`
/// 也忽略——那是给共享缓存的，本库的表是个人的（用户代理缓存），按 `RFC 9111 §5.2.2.9` 本就不该听它。
struct CacheDirectives {
    bool no_store{false};  ///< `no-store`：这次应答不进表，本次请求也不看表
    bool no_cache{false};  ///< `no-cache`：进表，但即刻过期（每次交付前都要问对端）
    bool must_revalidate{false};  ///< `must-revalidate`：过期后不得交付陈旧副本（本库缺省即如此）
    std::optional<std::chrono::seconds> max_age;  ///< `max-age`：显式新鲜期；缺失或不可解析为空
    // 本结构不含 `Expires`：那是另一枚字段（不在 `Cache-Control` 里），折算发生在 `freshness_of()` 现读应答头时。
};

/// @brief 一条已存应答的新鲜度事实（存入时折算完毕，判定不再解析字段）。
struct Freshness {
    std::chrono::seconds lifetime{0};  ///< 新鲜期；`0` = 抵达即过期
    std::int64_t initial_age{0};  ///< 抵达时刻该应答已经历的秒数（`Date` 与 `Age` 的折算结果）
    CacheTime arrived;  ///< 抵达时刻（判定基准，由发起侧注入）
};

/// @brief 表里的一条应答：交付形态 + 命中判定所需的全部事实。
struct CacheEntry {
    aurora::net::http::Method method{aurora::net::http::Method::Get};  ///< 建键方法
    std::string url;  ///< 建键 URL（发起侧给的原文，已解析过）
    std::vector<std::pair<std::string, std::string>> vary;  ///< `Vary` 点名的头名 + 当时请求里的取值
    /// @brief 已存应答（体已解压；`from_cache()` 恒假，命中时才现置真）。
    aurora::net::http::Response response{0, "", aurora::Headers{}, std::vector<char>{}};
    Freshness freshness;  ///< 新鲜度事实
    std::optional<std::string> etag;  ///< 校验子之一；缺失表示对端没给
    std::optional<std::string> last_modified;  ///< 校验子之二
    bool must_revalidate{false};  ///< 仅作诊断与后续 stale-XXX 分支的记录，本库缺省已满足其禁令
    std::uint64_t used{0};  ///< 最近一次使用序号（逐出用）
};

/// @brief 把同名多行字段并成一串逗号分隔的文本（`Cache-Control` / `Vary` 都允许分行给）。
///
/// 发起侧判读请求侧的 `Cache-Control` 时要用同一个合并口径：只取第一行会把「`no-store` 写在第二行」的请求
/// 读成普通请求，那与表内判定的结果不一致，两边的动作就分岔了。
/// @param headers 字段表。
/// @param name 字段名（大小写不敏感）。
/// @return 同名各行的值以逗号相连；无该字段时为空串。
[[nodiscard]] auto joined_field(const aurora::Headers &headers, std::string_view name) -> std::string;

/// @brief 解析 `Cache-Control` 字段（可多行、逗号分隔、`token=值` 大小写不敏感）。
///
/// 同一份里 `max-age` 出现多次取**最后一个**（后写覆盖先写，与字段顺序一致）；值带引号或不可解析按「没给」
/// 处理，不报错——本库不该因为对端多打了一个空格就拒绝缓存一条本来能缓存的应答。
/// @param value 字段原文（单行形态；多行由调用方先并成一串逗号分隔的文本）。
/// @return 认得的那几枚；其余指令不出现在结果里也不保留原名。
[[nodiscard]] auto parse_cache_control(std::string_view value) -> CacheDirectives;

/// @brief 状态码是否属于可缓存的那一族（见文件头第五条口径）。
/// @param status 三位状态码。
/// @return 可进表为 true。
[[nodiscard]] auto is_cacheable_status(std::uint16_t status) -> bool;

/// @brief 方法是否可进表（只 `GET` / `HEAD`）。
/// @param method 请求方法。
/// @return 可进表为 true。
[[nodiscard]] auto is_cacheable_method(aurora::net::http::Method method) -> bool;

/// @brief 解析 `Vary` 字段为被点名的头名列表（小写化，ASCII 大小写不敏感匹配的口径）。
/// @param value 字段原文；缺失时调用方不必问本函数。
/// @return 头名列表；含 `*` 时为空 `nullopt`，意为「这份应答不可复用」。
[[nodiscard]] auto parse_vary(std::string_view value) -> std::optional<std::vector<std::string>>;

/// @brief 把应答头里的 `Date` / `Age` / `Expires` 折算成新鲜度事实。
///
/// 初始年龄按 `RFC 9111 §4.2.3` 的三段算式收拢：`apparent_age = max(0, 抵达 - Date)`、
/// `corrected_age = Age + 请求滞留`（本库不测请求滞留，取 0 并在文档写明），两者取大后加「在本表里住了多久」。
/// 本函数只算到「抵达时刻的年龄」，居住时长由判定时现算，故条目可以长期躺在表里。
/// @param response 已物化的应答（读它的 `Cache-Control` / `Date` / `Age` / `Expires`）。
/// @param arrived 抵达时刻（由发起侧注入，使单测可复现）。
/// @return 新鲜度事实；`lifetime` 为 `0` 表示「无显式有效期」（调用方须另判有无校验子）。
[[nodiscard]] auto freshness_of(const aurora::net::http::Response &response, CacheTime arrived) -> Freshness;

/// @brief 同一套折算，但 `Cache-Control` 已经解析过（存入路径上一份字段只解一次）。
/// @param response 已物化的应答（读它的 `Date` / `Age` / `Expires`）。
/// @param arrived 抵达时刻。
/// @param directives 该应答的 `Cache-Control` 解析结果（`no-cache` 在此处折成「即刻过期」）。
/// @return 新鲜度事实。
[[nodiscard]] auto freshness_of(const aurora::net::http::Response &response, CacheTime arrived,
                                const CacheDirectives &directives) -> Freshness;

/// @brief 新鲜度判定：条目此刻是否还可以不经对端直接交付。
/// @param entry 表里的条目。
/// @param now 判定时刻（由发起侧注入）。
/// @return 新鲜为 true；`must_revalidate` 不改变本函数的判定（本库过期一律验证）。
[[nodiscard]] auto is_fresh(const CacheEntry &entry, CacheTime now) -> bool;

/// @brief 用重验证应答（304）的头部块更新已存头部块（`RFC 9111 §4.3.4`）。
///
/// 只并「应答头字段」：304 里出现的同名字段替换已存的那一枚，没出现的原样留着（304 不必重复整份头部）。
/// 四枚除外——`Content-Length` / `Content-Range` / `Trailer` / `Transfer-Encoding` 描述的是载荷本体，
/// 而载荷本体来自已存那份，让 304 的对应字段覆盖等于把「这条消息有多长」交给一次没有消息体的应答。
/// @param stored 已存的头部块。
/// @param update 304 交来的头部块。
/// @return 合并后的头部块（同名以 `update` 为准，其余保持 `stored` 的书写次序）。
[[nodiscard]] auto merge_revalidated_headers(const aurora::Headers &stored, const aurora::Headers &update)
    -> aurora::Headers;

/// @brief 一条请求在该表上的查找结果。
struct CacheLookup {
    std::optional<aurora::net::http::Response> fresh;  ///< 可直接交付的新鲜副本（已置 `from_cache()`）
    std::optional<CacheEntry> revalidate;  ///< 陈旧但带校验子的旧副本：发起侧据它挂两枚条件头
};

/// @brief 应答缓存表：按 (方法, URL, `Vary` 取值) 建键，超上限逐出最久未使用的一条。
///
/// 表是共享状态：`Client` 的拷贝共用同一张（各拿一份空表就等于把已付的代价扔掉）。查找是线性扫描，上限
/// 只有几十条，按 §8.8 的 hot-path 纪律这不需要索引结构；`Vary` 的匹配语义（同一 URL 可有多条变体）本身
/// 就容不下「一个键一条」的表。锁只护表本身，判定与折算都在锁外由自由函数完成。
class HttpResponseCache final {
  public:
    /// @brief 以给定口径构造（`max_entries` 决定容量；`enabled` 由发起侧判，本类不读它）。
    /// @param options 缓存口径。
    explicit HttpResponseCache(aurora::net::http::CacheOptions options);

    HttpResponseCache(const HttpResponseCache &) = delete;
    auto operator=(const HttpResponseCache &) -> HttpResponseCache & = delete;
    HttpResponseCache(HttpResponseCache &&) = delete;
    auto operator=(HttpResponseCache &&) -> HttpResponseCache & = delete;
    ~HttpResponseCache() = default;

    /// @brief 改口径（容量变化时按新上限逐出多余条目）。
    /// @param options 新口径。
    auto set_options(aurora::net::http::CacheOptions options) -> void;

    /// @brief 查一次表：新鲜副本，或可用于重验证的旧副本。
    /// @param method 请求方法。
    /// @param url 目标 URL 文本（发起侧已解析，取其规范化渲染）。
    /// @param request_headers 本次请求的字段表（`Vary` 匹配要看它）。
    /// @param now 判定时刻。
    /// @param extra_vary 发起侧追加的变体名（钩子触点集）：条目没把它算进键即**不命中**。
    /// @return 两者至多其一非空；都没匹配上时两者皆空。
    [[nodiscard]] auto lookup(aurora::net::http::Method method, const std::string &url,
                              const aurora::Headers &request_headers, CacheTime now,
                              const std::vector<std::string> &extra_vary = {}) -> CacheLookup;

    /// @brief 把一次应答按规则存进表（规则全在本函数判，调用方无须预筛）。
    /// @param method 产出该应答的请求方法。
    /// @param url 该应答所属的目标 URL。
    /// @param request_headers 当时的请求字段（`Vary` 取值从这里取）。
    /// @param response 已物化的应答（体已解压）。
    /// @param now 抵达时刻。
    /// @param extra_vary 发起侧追加的变体名（钩子触点集），与应答 `Vary` 及凭据名去重后一并存入。
    /// @return 进了表为 true；不可存（`no-store` / `Vary: *` / 方法或状态码不可缓存 / 无有效期又无校验子）
    ///         为 false，表保持原样。
    [[nodiscard]] auto store(aurora::net::http::Method method, const std::string &url,
                             const aurora::Headers &request_headers, const aurora::net::http::Response &response,
                             CacheTime now, const std::vector<std::string> &extra_vary = {}) -> bool;

    /// @brief 重验证成功（拿到 304）：并头部、重新折算新鲜度、换回一份可直接交付的应答。
    /// @param method 请求方法。
    /// @param url 目标 URL。
    /// @param request_headers 本次请求字段（`Vary` 匹配要看它）。
    /// @param not_modified 对端交回的 304（只用它的头部块）。
    /// @param now 304 的抵达时刻。
    /// @param extra_vary 发起侧追加的变体名（钩子触点集），匹配口径与 `lookup` 同一条。
    /// @return 命中并合并成功为可交付的应答（`from_cache()` 为真）；表里没有对应条目为空。
    [[nodiscard]] auto revalidated(aurora::net::http::Method method, const std::string &url,
                                   const aurora::Headers &request_headers,
                                   const aurora::net::http::Response &not_modified, CacheTime now,
                                   const std::vector<std::string> &extra_vary = {})
        -> std::optional<aurora::net::http::Response>;

    /// @brief 当前条目数（判据与观测用）。
    /// @return 条数。
    [[nodiscard]] auto size() const -> std::size_t;

    /// @brief 清空表（开关关掉不自动清，调用方要清时用；本切片不提供逐条失效的入口）。
    auto clear() -> void;

  private:
    /// @brief 找匹配条目：键同且 `Vary` 点名的头取值全等。
    ///
    /// 两道判定有先后：先查「条目是否把当前每一枚追加变体名算进了键」——缺名即不匹配，另一租户的副本
    /// 绝不能被当成答案；再做既有的逐值比较（追加名存入时连同取值进了 `vary`，故这里不需要第二条比较）。
    /// @param method 请求方法。
    /// @param url 目标 URL。
    /// @param request_headers 本次请求字段。
    /// @param extra_vary 发起侧追加的变体名。
    /// @return 下标；无匹配为空。
    [[nodiscard]] auto find_index(aurora::net::http::Method method, const std::string &url,
                                  const aurora::Headers &request_headers,
                                  const std::vector<std::string> &extra_vary) const -> std::optional<std::size_t>;

    /// @brief 越上限时逐出最久未使用的条目，直到表内条数不超过 `options_.max_entries`。
    auto evict_over_capacity() -> void;

    mutable std::mutex mutex_;  ///< 护表；判定与折算不在锁内做
    aurora::net::http::CacheOptions options_;  ///< 口径（容量取这里的 `max_entries`）
    std::vector<CacheEntry> entries_;  ///< 表体
    std::uint64_t clock_{0};  ///< 使用序号（逐出依据）
};

}  // namespace aurora::net::detail
