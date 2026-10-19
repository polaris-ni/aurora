// resolver.cpp —— 主机名解析的编排面（本文件不含任何平台 socket 头）。
//
// 分层纪律：`getaddrinfo` 属平台 socket 头，仓内唯一落点是 `src/aurora/net/socket.cpp`
// （不变量），故本文件只做「字面量短路 → 取列表 → 规范化」的编排，平台腿经
// `net::detail::platform_lookup` 调用。异步腿复用 `net::Engine` 的常驻线程做阻塞查询，完成
// 投递走 `aurora::detail::post_to_main`——与 `Task<T>` 同一条主线程通道，因此宿主装了自有
// 投递器时网络完成与 UI 事件共用一个队列（不新增第三种排队形态）。
//
// 注意公共头的依赖方向：本 .cpp 可以消费 `state/`（实现侧），`net/resolver.h` 不行——解析器
// 在 L1，`Resource` 一类绑定层在 L4，把 `Task` 写进公共头会让下层反向依赖上层。

#include "aurora/net/resolver.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/lookup.h"
#include "aurora/net/engine.h"
#include "aurora/state/async.h"

namespace aurora::net {
namespace {

/// @brief 去重并保持首次出现次序（同族同地址同端口只留第一条）。
///
/// 平台解析器常把同一地址以两条记录回报（v4-mapped 与 v4 各一条、或 A 与 CNAME 指向同值），
/// 不去重会让调用方对同一台机器发起重复连接尝试。列表规模恒为个位数，故用线性查找而非哈希
/// ——哈希会把「首次出现次序」变成不确定项，而那正是本层要保住的唯一事实。
/// @param list 待收敛的地址列表（就地取出，函数接管其存储）。
/// @return 收敛后的列表。
auto dedup(std::vector<Endpoint> list) -> std::vector<Endpoint> {
    std::vector<Endpoint> out;
    out.reserve(list.size());
    for (auto &item : list) {
        const auto duplicate = std::any_of(out.begin(), out.end(), [&](const Endpoint &kept) {
            return kept.family() == item.family() && kept.address_bytes() == item.address_bytes() &&
                   kept.port() == item.port();
        });
        if (!duplicate) {
            out.push_back(std::move(item));
        }
    }
    return out;
}

/// @brief 把结果交给回调，投递点恒为主线程投递器（未装即当前线程就地执行）。
///
/// 「恰好一次」由这里唯一保证：交出即清空持有的回调，故两条投递路径（引擎线程完成、引擎
/// 不可用就地失败）即便都会触发也只执行一次。
/// @param slot 回调槽（共享，因两条路径都可能持有）。
/// @param result 解析结果。
auto deliver(const std::shared_ptr<ResolveDone> &slot, Result<std::vector<Endpoint>> result) -> void {
    aurora::detail::post_to_main([slot, payload = std::move(result)]() mutable {
        if (!*slot) {
            return;  // 已投递过：不二次触发
        }
        auto callback = std::move(*slot);
        callback(std::move(payload));
    });
}

}  // namespace

namespace detail {
namespace {

/// @brief 钩子的唯一存储点（两枚出入口共用同一个槽，故装入必被读到）。
///
/// 函数级静态而非命名空间级：库以静态库形态交付，跨 TU 的动态初始化次序不可控，而这里是
/// 运行期可变状态。
/// @return 槽的可变引用。
auto hook_slot() -> LookupFn & {
    static LookupFn slot;
    return slot;
}

}  // namespace

auto set_lookup_hook(LookupFn hook) -> void { hook_slot() = std::move(hook); }

auto lookup_hook() -> const LookupFn & { return hook_slot(); }

}  // namespace detail

auto Resolver::lookup(std::string_view host, std::uint16_t port) -> Result<std::vector<Endpoint>> {
    if (host.empty()) {
        // 用法错误不落网络故障码：`net-dns-failed` 的 hint 会让调用方去重试一个永远不会成功的入参。
        return make_error(ErrorCode::NetEndpointInvalid, "name resolution needs a non-empty host");
    }
    // 字面量短路在钩子**之前**：注入接缝只替换「查网」这条腿，因此测试能同时证明字面量没走 DNS。
    if (auto literal = Endpoint::from_ip(host, port)) {
        return std::vector<Endpoint>{std::move(literal.value())};
    }
    const auto &hook = detail::lookup_hook();
    auto list = hook ? hook(host, port) : detail::platform_lookup(host, port);
    if (!list) {
        return list.error();
    }
    auto normalized = dedup(std::move(list.value()));
    if (normalized.empty()) {
        return make_error(ErrorCode::NetDnsFailed,
                          "host=" + std::string{host} + ", resolver returned no usable address record");
    }
    return normalized;
}

auto Resolver::resolve(std::string_view host, std::uint16_t port, ResolveDone on_done) -> void {
    if (!on_done) {
        return;  // 空回调即「不想知道结果」，此时连查表都不发起，比查完再丢弃更诚实
    }
    auto slot = std::make_shared<ResolveDone>(std::move(on_done));
    auto queued = Engine::instance().post(
        [host_text = std::string(host), port, slot] { deliver(slot, Resolver::lookup(host_text, port)); });
    if (!queued) {
        // 不静默悬挂：把引擎的失败原因原样带进回调，并标明是排队失败而非解析失败。
        deliver(slot,
                make_error(queued.error().code_enum, "name resolution could not be queued: " + queued.error().message));
    }
}

}  // namespace aurora::net
