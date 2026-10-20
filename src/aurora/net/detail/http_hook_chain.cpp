// http_hook_chain.cpp —— 交换钩子链的库内内核（见同名头的文件头口径）。
#include "aurora/net/detail/http_hook_chain.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/http_request_id.h"
#include "aurora/net/http.h"
#include "aurora/net/http_hook.h"

namespace aurora::net::detail {
namespace {

/// @brief 保留头：第二列是点名用的规范拼写，第一列是大小写不敏感的匹配键。
struct ReservedField {
    std::string_view key;  ///< ASCII 小写匹配键
    std::string_view name;  ///< 规范拼写（detail 只提它，不提值）
};

/// @brief 所有腿共用的保留头（判据见 `net/http_hook.h` 的四组表）。
constexpr ReservedField AURORA_RESERVED_REQUEST_FIELDS[] = {
    {.key = "host", .name = "Host"},
    {.key = "x-request-id", .name = "X-Request-Id"},
    {.key = "content-length", .name = "Content-Length"},
    {.key = "transfer-encoding", .name = "Transfer-Encoding"},
    {.key = "content-encoding", .name = "Content-Encoding"},
    {.key = "connection", .name = "Connection"},
    {.key = "te", .name = "TE"},
    {.key = "trailer", .name = "Trailer"},
    {.key = "upgrade", .name = "Upgrade"},
    {.key = "range", .name = "Range"},
    {.key = "if-range", .name = "If-Range"},
    {.key = "expect", .name = "Expect"},
    {.key = "proxy-authorization", .name = "Proxy-Authorization"},
};

/// @brief 仅 WebSocket 腿额外的四枚：握手字段由库写定，子协议与扩展走 `WebSocketOptions`。
constexpr ReservedField AURORA_WS_RESERVED_REQUEST_FIELDS[] = {
    {.key = "sec-websocket-key", .name = "Sec-WebSocket-Key"},
    {.key = "sec-websocket-version", .name = "Sec-WebSocket-Version"},
    {.key = "sec-websocket-protocol", .name = "Sec-WebSocket-Protocol"},
    {.key = "sec-websocket-extensions", .name = "Sec-WebSocket-Extensions"},
};

/// @brief ASCII 小写化（字段名的比较与触点集的键都用这一条尺）。
/// @param text 原文。
/// @return 逐字节折叠大写字母后的副本。
[[nodiscard]] auto to_lower_ascii(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char unit : text) {
        const auto raw = static_cast<unsigned char>(unit);
        out.push_back(raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')
                          ? static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'))
                          : unit);
    }
    return out;
}

/// @brief 腿的线上称呼（只用于 WARN 与 detail，不进任何协议字段）。
/// @param leg 腿。
/// @return 短名。
[[nodiscard]] auto leg_name(http::ExchangeLeg leg) -> std::string_view {
    switch (leg) {
        case http::ExchangeLeg::Send:
            return "send";
        case http::ExchangeLeg::Stream:
            return "stream";
        case http::ExchangeLeg::ServerSentEvents:
            return "server-sent-events";
        case http::ExchangeLeg::WebSocket:
            return "websocket";
    }
    return "unknown";
}

/// @brief 名字 → 值序列的归一视图：同名的多行按写入次序收在一起，名字一律 ASCII 小写。
using FieldValues = std::vector<std::pair<std::string, std::vector<std::string>>>;

/// @brief 把一份字段表折成「小写名 → 值序列」。
/// @param headers 字段表。
/// @return 按首次出现次序排列的条目；同名多行合入同一枚值序列。
[[nodiscard]] auto collect_fields(const Headers &headers) -> FieldValues {
    FieldValues out;
    for (const auto &entry : headers.entries()) {
        std::string name = to_lower_ascii(entry.name);
        const auto found = std::ranges::find_if(
            out, [&name](const std::pair<std::string, std::vector<std::string>> &item) { return item.first == name; });
        if (found == out.end()) {
            out.emplace_back(std::move(name), std::vector<std::string>{entry.value});
        } else {
            found->second.push_back(entry.value);
        }
    }
    return out;
}

/// @brief 两份字段表之间的全部改动名（小写，按先行后列的出现次序，不重复）。
///
/// 判据是「名 → 值序列」的逐名比较：新增、删除、改值、同名多行的次序或条数变化都算一处改动。
/// @param before 改动前。
/// @param after 改动后。
/// @return 改动名列表；无改动时为空。
[[nodiscard]] auto changed_fields(const Headers &before, const Headers &after) -> std::vector<std::string> {
    const FieldValues left = collect_fields(before);
    const FieldValues right = collect_fields(after);
    std::vector<std::string> changed;
    for (const auto &[name, values] : left) {
        const auto found = std::ranges::find_if(right, [&name](const auto &item) { return item.first == name; });
        if (found == right.end() || found->second != values) {
            changed.push_back(name);
        }
    }
    for (const auto &[name, values] : right) {
        if (std::ranges::none_of(left, [&name](const auto &item) { return item.first == name; })) {
            changed.push_back(name);
        }
    }
    return changed;
}

/// @brief 改动名是否落在本腿的保留头集合里。
/// @param name 小写改动名。
/// @param leg 本腿。
/// @return 命中时返回规范拼写；否则 `nullopt`。
[[nodiscard]] auto reserved_field(std::string_view name, http::ExchangeLeg leg) -> std::optional<std::string_view> {
    const auto matches = [name](const ReservedField &field) { return field.key == name; };
    if (const auto *const found = std::ranges::find_if(std::begin(AURORA_RESERVED_REQUEST_FIELDS),
                                                       std::end(AURORA_RESERVED_REQUEST_FIELDS), matches);
        found != std::end(AURORA_RESERVED_REQUEST_FIELDS)) {
        return found->name;
    }
    if (leg == http::ExchangeLeg::WebSocket) {
        if (const auto *const found = std::ranges::find_if(std::begin(AURORA_WS_RESERVED_REQUEST_FIELDS),
                                                           std::end(AURORA_WS_RESERVED_REQUEST_FIELDS), matches);
            found != std::end(AURORA_WS_RESERVED_REQUEST_FIELDS)) {
            return found->name;
        }
    }
    return std::nullopt;
}

/// @brief 钩子回调期间的线程局部栈：正在跑两问的钩子指针。
///
/// 函数内 `thread_local` 惰性构造：只有真的挂过钩子的线程才付这份存储，且不落一枚非 const 全局变量。
/// @return 本线程的栈。
[[nodiscard]] auto hook_stack() -> std::vector<const http::ExchangeHook *> & {
    thread_local std::vector<const http::ExchangeHook *> stack;
    return stack;
}

/// @brief 回调入口压栈、出口（含异常展开）弹栈的 RAII。
/// @note Thread: 随调用线程；一份 guard 只活在一次同步回调里
/// @note Side-effects: 改本线程的钩子栈
class HookStackGuard {
  public:
    /// @brief 压栈。
    /// @param hook 即将回调的钩子。
    explicit HookStackGuard(const http::ExchangeHook *hook) : hook_{hook} { hook_stack().push_back(hook); }

    HookStackGuard(const HookStackGuard &) = delete;
    auto operator=(const HookStackGuard &) -> HookStackGuard & = delete;
    HookStackGuard(HookStackGuard &&) = delete;
    auto operator=(HookStackGuard &&) -> HookStackGuard & = delete;

    /// @brief 弹栈（末尾那枚一定是自己：两问的调用是严格嵌套的）。
    ~HookStackGuard() {
        auto &stack = hook_stack();
        if (!stack.empty()) {
            stack.pop_back();
        }
    }

  private:
    const http::ExchangeHook *hook_{nullptr};  ///< 本层钩子
};

/// @brief 构造一条拒绝。
/// @param detail_ascii 只含可公开事实的说明（钩子序号、腿、保留头名；不含 URL、头值、异常原文）。
/// @return `http-request-invalid`。
[[nodiscard]] auto reject(std::string detail_ascii) -> Error {
    return make_error(ErrorCode::HttpRequestInvalid, ErrorParams{{"detail", std::move(detail_ascii)}});
}

/// @brief 取本次交换的链路标识（只用于 WARN；未打标时为空串）。
/// @param headers 当前请求头。
/// @return 标识文本。
[[nodiscard]] auto exchange_identity(const Headers &headers) -> std::string {
    return headers.get(detail::AURORA_HTTP_REQUEST_ID).value_or(std::string{});
}

}  // namespace

auto ExchangeHookChain::use(std::shared_ptr<http::ExchangeHook> hook) -> http::ExchangeHookRegistration {
    if (!hook) {
        return {};  // 空指针即一次空登记：链上无条目，句柄可照常析构
    }
    const auto id = add(std::move(hook));
    // 句柄只持弱引用：所有 `Client` 都没了，链即销毁，句柄析构为空操作。
    return http::ExchangeHookRegistration{weak_from_this(), id};
}

auto ExchangeHookChain::add(std::shared_ptr<http::ExchangeHook> hook) -> std::uint64_t {
    if (!hook) {
        return 0;
    }
    const std::scoped_lock guard{mutex_};
    const std::uint64_t id = next_id_++;
    entries_.push_back(Entry{.id = id, .hook = std::move(hook)});
    registered_.store(entries_.size(), std::memory_order_release);
    return id;
}

auto ExchangeHookChain::remove(std::uint64_t id) -> void {
    if (id == 0) {
        return;
    }
    const std::scoped_lock guard{mutex_};
    const auto before = entries_.size();
    std::erase_if(entries_, [id](const Entry &entry) { return entry.id == id; });
    if (entries_.size() != before) {
        registered_.store(entries_.size(), std::memory_order_release);
    }
}

auto ExchangeHookChain::absent() const noexcept -> bool { return registered_.load(std::memory_order_acquire) == 0; }

auto ExchangeHookChain::run_before(http::Method method, const std::optional<Url> &url, Headers &headers,
                                   http::ExchangeLeg leg) const -> BeforeResult {
    BeforeResult out;
    if (absent()) {
        return out;  // 空链快路径：不加锁、不分配、不构造视图
    }
    std::vector<std::shared_ptr<http::ExchangeHook>> hooks;
    {
        const std::scoped_lock guard{mutex_};
        hooks.reserve(entries_.size());
        for (const auto &entry : entries_) {
            hooks.push_back(entry.hook);  // 快照：此后登记与注销都不影响本次交换
        }
    }
    if (hooks.empty()) {
        return out;  // 与取快照并发的一次注销把链清空了：按空链处理
    }
    out.run.engaged = true;
    const Headers chain_before = headers;  // 链末保留头终检的对照（逐钩子检查之外的冗余防线）
    std::vector<const http::ExchangeHook *> warned;  // 本次交换里已记过「跳过」的那些钩子
    const auto identity = exchange_identity(headers);

    for (std::size_t index = 0; index < hooks.size(); ++index) {
        const auto &hook = hooks[index];
        const auto &stack = hook_stack();
        if (std::ranges::find(stack, hook.get()) != stack.end()) {
            // 同一线程的同步重入：跳过这一枚，链上其余各枚照常跑；一次交换里同一枚只记一行。
            if (std::ranges::find(warned, hook.get()) == warned.end()) {
                warned.push_back(hook.get());
                AURORA_LOG_WARN("net", "exchange hook skipped on re-entry: hook #", index, ", leg ", leg_name(leg),
                                ", request id ", identity);
            }
            continue;
        }

        const Headers entry_headers = headers;  // 本枚钩子的入口快照：视图与差分都看它
        const http::ExchangeRequestView view{method, url, entry_headers, leg};
        std::optional<Result<http::Response>> delivery;
        {
            HookStackGuard guard{hook.get()};
            try {
                delivery = hook->before_request(view, headers);
            } catch (...) {
                // 半截改写不发给对端：后续各枚不再问，收摊问对谁也不问。
                out.status = BeforeStatus::Rejected;
                out.rejection = reject("exchange hook #" + std::to_string(index) + " on the " +
                                       std::string{leg_name(leg)} + " leg threw from before_request");
                return out;
            }
        }

        const auto changed = changed_fields(entry_headers, headers);
        for (const auto &name : changed) {
            if (const auto field = reserved_field(name, leg); field.has_value()) {
                out.status = BeforeStatus::Rejected;
                out.rejection =
                    reject("exchange hook #" + std::to_string(index) + " on the " + std::string{leg_name(leg)} +
                           " leg modified the reserved header '" + std::string{field.value()} + "'");
                return out;
            }
        }
        for (const auto &name : changed) {
            if (std::ranges::find(out.run.touched, name) == out.run.touched.end()) {
                out.run.touched.push_back(name);  // 链上并集，不回退
            }
        }
        out.run.asked.push_back(hook);

        if (!delivery.has_value()) {
            continue;
        }
        // 短路：只有 `send` 承认；静默忽略会让宿主的替身「以为 mock 了，实际打到了真网」。
        if (leg != http::ExchangeLeg::Send) {
            out.status = BeforeStatus::Rejected;
            out.rejection = reject("exchange hook #" + std::to_string(index) + " on the " + std::string{leg_name(leg)} +
                                   " leg returned a short-circuit from before_request; short-circuit responses are "
                                   "only honored on the send leg");
            return out;
        }
        if (delivery->ok() && delivery->value().from_cache()) {
            out.status = BeforeStatus::Rejected;
            out.rejection = reject("exchange hook #" + std::to_string(index) + " on the " + std::string{leg_name(leg)} +
                                   " leg returned a short-circuit response with from_cache set; a hook may not forge "
                                   "a cache hit");
            return out;
        }
        out.status = BeforeStatus::ShortCircuit;
        out.delivery = std::move(delivery);
        out.run.logical_headers = headers;
        return out;
    }

    // 链末保留头终检：逐钩子检查已是 fail-fast 的守卫，本段是结构不变量与未来扩展的防线。
    for (const auto &name : changed_fields(chain_before, headers)) {
        if (const auto field = reserved_field(name, leg); field.has_value()) {
            out.status = BeforeStatus::Rejected;
            out.rejection =
                reject("the final request after the exchange hook chain on the " + std::string{leg_name(leg)} +
                       " leg modified the reserved header '" + std::string{field.value()} + "'");
            return out;
        }
    }
    out.run.logical_headers = headers;  // 收摊视图：钩子改写后的原始逻辑头（不含库内稍后补的字段）
    return out;
}

// `noexcept` 是这条面的契约而不是优化提示：收摊问不得把一次已经成功的交换反转为失败。函数体内仍有三处理
// 论上能抛出（视图复制字段表、`warned` 追加、WARN 取 id 时的字符串拼装），三者只会因内存耗尽而抛——与流式
// 传输派发边界那几处同一口径：与其在终止与「静默吞掉分配失败」之间二选一，本层按既有豁免逐点标注，让抛出
// 走库侧统一的 terminate 路径。
// NOLINTBEGIN(bugprone-exception-escape)
auto ExchangeHookChain::run_after(const Run &run, http::Method method, const std::optional<Url> &url,
                                  http::ExchangeLeg leg, const http::Response &response) noexcept -> void {
    if (!run.engaged || run.asked.empty()) {
        return;
    }
    const auto identity = exchange_identity(run.logical_headers);
    const http::ExchangeRequestView view{method, url, run.logical_headers, leg};
    std::vector<const http::ExchangeHook *> warned;
    for (std::size_t index = 0; index < run.asked.size(); ++index) {
        const auto &hook = run.asked[index];
        const auto &stack = hook_stack();
        if (std::ranges::find(stack, hook.get()) != stack.end()) {
            if (std::ranges::find(warned, hook.get()) == warned.end()) {
                warned.push_back(hook.get());
                AURORA_LOG_WARN("net", "exchange hook skipped on re-entry: hook #", index, ", leg ", leg_name(leg),
                                ", request id ", identity);
            }
            continue;
        }
        HookStackGuard guard{hook.get()};
        try {
            hook->after_response(view, response);
        } catch (...) {
            // 收摊问是观测面，不得反向失败一次已经成功的交换；异常原文不进日志（脱敏纪律）。
            AURORA_LOG_WARN("net", "exchange hook threw from after_response: hook #", index, ", leg ", leg_name(leg),
                            ", request id ", identity);
        }
    }
}
// NOLINTEND(bugprone-exception-escape)

}  // namespace aurora::net::detail
