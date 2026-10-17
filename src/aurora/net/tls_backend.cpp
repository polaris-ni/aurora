// Aurora net — TLS 后端注册表与候选链解析。
//
// 本文件是「这一次握手用哪条 TLS 腿」的唯一决策点。它之前，构建配置只决定「哪些腿在案」（每枚腿
// 一个 TU、各自带一个唯一名的工厂）；它之后，会话状态机（`net/tls.cpp`）只认一枚已经造出来的后端，
// 不再有第二次选择的机会。
//
// 为什么回落只发生在后端**造出来之前**：候选不可用是一个可以在握手前判定的事实（没注册、或这份
// 口径它表达不了），而握手半途失败不是——各腿的信任根各在自己的实现里，换一条腿等于换一次证书
// 校验的口径，那会把安全判定交给调度器。所以本文件只对两类明确「这一枚不行」的错误码继续下一枚，
// 其余失败原样终止整条链。
//
// 锁纪律：注册表用一枚互斥量串行化，所有读取都返回**快照副本**，绝不在持锁期间调用工厂——工厂是
// 调用方提供的任意代码，它完全可能在回调里注册或注销后端，持锁调用就是一处自死锁。测试隔离用的整表
// 快照槽同样归这枚互斥量管，它只在锁内读写，因此不存在「还原到一半被解析看见」的中间态。

#include "aurora/net/tls_backend.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/net/detail/tls_backend.h"

namespace aurora::net {
namespace {

// 内置后端的标识。名字即构建宏所指的这条腿，写进诊断后调用方可以直接拿去填 `TlsContext::backend`。
constexpr std::string_view AURORA_ID_SCHANNEL = "schannel";
constexpr std::string_view AURORA_ID_SECURE_TRANSPORT = "securetransport";
constexpr std::string_view AURORA_ID_OPENSSL = "openssl";
constexpr std::string_view AURORA_ID_MBEDTLS = "mbedtls";

// 内置腿的缺省次序：系统栈在前（零依赖、补丁与信任根都归操作系统管），provider 库按「更通用者在前」
// 排。次序是缺省值而非强制——`set_default_tls_backends()` 可以整体改写。
constexpr int AURORA_PRIORITY_SYSTEM = 100;
constexpr int AURORA_PRIORITY_OPENSSL = 60;
constexpr int AURORA_PRIORITY_MBEDTLS = 40;

auto make_detail(ErrorCode code, std::string text) -> Error {
    return make_error(code, ErrorParams{{"detail", std::move(text)}});
}

// `general-invalid-argument` 的表模板是「Invalid argument supplied」，没有 `{detail}` 槽位：这类错误走
// 模板渲染等于把「哪一项不对」整段丢掉，而注册表拒绝入参时调用方唯一需要的就是那一段。故按覆盖 message
// 的重载构造——severity/category/retryable 仍来自表，只有文本由调用方给全。
auto make_invalid(std::string_view text) -> Error {
    return make_error(ErrorCode::GeneralInvalidArgument, std::string{text});
}

// 一条在案的后端。
struct Entry {
    std::string id;  ///< 稳定标识
    int priority = 0;  ///< 缺省链次序，大者在前
    TlsBackendCreator create;  ///< 工厂
};

// 注册表本体：函数内静态，首次访问即把构建选中的那几枚内置腿装进去（装配集合与构建宏严格同源）。
struct Registry {
    std::mutex mutex;  ///< 全部状态的唯一串行化点
    std::vector<Entry> entries;  ///< 在案后端（注册次序保存，展示时另排）
    std::vector<std::string> chain;  ///< 全局缺省候选链
    bool chain_set = false;  ///< 链是否被显式置过（未置即按优先级动态推导）
    bool builtins_installed = false;  ///< 内置腿是否已装入
};

auto registry() -> Registry & {
    static Registry instance;
    return instance;
}

// 整表快照槽：只为一件事存在——同进程里先后跑的测试用例能把注册表现场原样还回去。槽内状态由
// 注册表那枚互斥量保护，故它与「解析在跑」之间不会互相看见半成品。
struct RegistrySnapshot {
    std::vector<Entry> entries;  ///< 在案后端（含工厂）
    std::vector<std::string> chain;  ///< 缺省候选链
    bool chain_set = false;  ///< 链是否被显式置过
    bool taken = false;  ///< 槽是否已填过（未填时还原即空操作）
};

auto snapshot_slot() -> RegistrySnapshot & {
    static RegistrySnapshot slot;
    return slot;
}

auto install_builtins_locked(Registry &state) -> void {
    if (state.builtins_installed) {
        return;
    }
    state.builtins_installed = true;
    // `maybe_unused`：一枚后端也没编进来的构建里，下面四支 `#if` 全不成立，这枚 lambda 只声明不调用。
    [[maybe_unused]] auto add = [&state](std::string_view id, int priority, TlsBackendCreator creator) {
        state.entries.push_back(Entry{.id = std::string{id}, .priority = priority, .create = std::move(creator)});
    };
#ifdef AURORA_ENABLE_NET_TLS_SCHANNEL
    add(AURORA_ID_SCHANNEL, AURORA_PRIORITY_SYSTEM, &detail::make_tls_backend_schannel);
#endif
#ifdef AURORA_ENABLE_NET_TLS_SECURE_TRANSPORT
    add(AURORA_ID_SECURE_TRANSPORT, AURORA_PRIORITY_SYSTEM, &detail::make_tls_backend_securetransport);
#endif
#ifdef AURORA_ENABLE_NET_TLS_OPENSSL
    add(AURORA_ID_OPENSSL, AURORA_PRIORITY_OPENSSL, &detail::make_tls_backend_openssl);
#endif
#ifdef AURORA_ENABLE_NET_TLS_MBEDTLS
    add(AURORA_ID_MBEDTLS, AURORA_PRIORITY_MBEDTLS, &detail::make_tls_backend_mbedtls);
#endif
}

// 在快照里按 id 找一条后端。
auto find_locked(const std::vector<Entry> &entries, std::string_view id) -> const Entry * {
    const auto found =
        std::find_if(entries.begin(), entries.end(), [id](const Entry &entry) { return entry.id == id; });
    return found == entries.end() ? nullptr : &*found;
}

// 缺省链 = 在案后端按优先级降序（同优先级保持注册次序）。
auto chain_from(const std::vector<Entry> &entries) -> std::vector<std::string> {
    std::vector<Entry> sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Entry &left, const Entry &right) { return left.priority > right.priority; });
    std::vector<std::string> ids;
    ids.reserve(sorted.size());
    for (const auto &entry : sorted) {
        ids.push_back(entry.id);
    }
    return ids;
}

// 只回答「这一枚不行」的错误码才可以触发下一枚候选；其余错误说的是「这份配置不行」，换后端只会把
// 同一个错再犯一遍。
auto is_skip_only(ErrorCode code) -> bool {
    return code == ErrorCode::NetTlsEngineUnavailable || code == ErrorCode::NetPlatformUnavailable;
}

auto joined(const std::vector<std::string> &ids) -> std::string {
    std::string text;
    for (const auto &id : ids) {
        if (!text.empty()) {
            text += ", ";
        }
        text += id;
    }
    return text;
}

using Resolved = Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>;

}  // namespace

auto register_tls_backend(const TlsBackendRegistration &registration) -> Result<void> {
    if (registration.id.empty()) {
        return make_invalid("a tls backend registration needs a non-empty id");
    }
    if (!registration.create) {
        return make_invalid("tls backend '" + registration.id + "' was registered without a factory");
    }
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    install_builtins_locked(state);
    // 不覆盖：内置腿与别的模块可能正按同一 id 被引用，静默替换让「谁在跑这条连接」变成一个查不出来的
    // 事实。想换就先 unregister。
    if (find_locked(state.entries, registration.id) != nullptr) {
        return make_detail(ErrorCode::NetOptionConflict,
                           "tls backend '" + registration.id + "' is already in the registry: unregister it first");
    }
    state.entries.push_back(
        Entry{.id = registration.id, .priority = registration.priority, .create = registration.create});
    return Result<void>{};
}

auto unregister_tls_backend(std::string_view id) -> bool {
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    install_builtins_locked(state);
    const auto found =
        std::find_if(state.entries.begin(), state.entries.end(), [id](const Entry &entry) { return entry.id == id; });
    if (found == state.entries.end()) {
        return false;
    }
    state.entries.erase(found);
    return true;
}

auto available_tls_backends() -> std::vector<std::string> {
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    install_builtins_locked(state);
    return chain_from(state.entries);
}

auto set_default_tls_backends(std::vector<std::string> chain) -> Result<void> {
    for (std::size_t index = 0; index < chain.size(); ++index) {
        if (chain[index].empty()) {
            // 说「第 index 项」而不是「有个空串」：链是有次序的，调用方要立刻知道是哪一项写漏了。
            return make_invalid("the tls candidate chain holds an empty backend id at index " + std::to_string(index));
        }
    }
    auto sorted = chain;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return make_invalid(
            "the tls candidate chain lists the same backend id twice (order carries meaning, "
            "duplicates do not)");
    }
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    state.chain = std::move(chain);
    state.chain_set = true;
    return Result<void>{};
}

auto default_tls_backends() -> std::vector<std::string> {
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    install_builtins_locked(state);
    return state.chain_set ? state.chain : chain_from(state.entries);
}

auto detail::tls_backend_available() -> bool { return !available_tls_backends().empty(); }

auto detail::snapshot_tls_backend_registry() -> void {
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    install_builtins_locked(state);
    auto &slot = snapshot_slot();
    slot.entries = state.entries;
    slot.chain = state.chain;
    slot.chain_set = state.chain_set;
    slot.taken = true;
}

auto detail::restore_tls_backend_registry() -> void {
    auto &state = registry();
    std::scoped_lock lock{state.mutex};
    auto &slot = snapshot_slot();
    if (!slot.taken) {
        return;
    }
    state.entries = slot.entries;
    state.chain = slot.chain;
    state.chain_set = slot.chain_set;
}

auto detail::resolve_tls_backend(const TlsContext &context, std::string_view server_name) -> Resolved {
    std::vector<Entry> registered;
    std::vector<std::string> chain;
    {
        auto &state = registry();
        std::scoped_lock lock{state.mutex};
        install_builtins_locked(state);
        registered = state.entries;
        chain = state.chain_set ? state.chain : chain_from(state.entries);
    }

    const bool designated = context.backend.has_value();
    if (designated) {
        chain = std::vector<std::string>{*context.backend};
    }
    if (chain.empty()) {
        // 空链是调用方把缺省链置成了空表，与「有链但一条都不在案」是两件事，故诊断分开说：这一条教的是
        // 去置链或注册，而不是去改构建。
        return Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>{
            make_detail(ErrorCode::NetTlsEngineUnavailable,
                        "the tls candidate chain is empty and no backend was designated: call set_default_tls_backends "
                        "or register one (in the registry: " +
                            joined(chain_from(registered)) + ")")};
    }

    // 最后一枚跳过原因：全链被跳过时用它作结局，因为「试过了但都不行」比凭空造一句更有现场价值。
    Error exhausted =
        make_detail(ErrorCode::NetTlsEngineUnavailable,
                    "no tls backend in the candidate chain can serve this request (tried: " + joined(chain) +
                        "; in the registry: " + joined(chain_from(registered)) + ")");
    for (const auto &wanted : chain) {
        const Entry *found = find_locked(registered, wanted);
        if (found == nullptr) {
            auto reason = make_detail(ErrorCode::NetTlsEngineUnavailable, "tls backend '" + wanted +
                                                                              "' is not registered (in the registry: " +
                                                                              joined(chain_from(registered)) + ")");
            if (designated) {
                // 指名即用：写错 id 与「那条腿今天不在案」必须是同一个明确失败，不能被下一枚后端接走。
                return Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>{std::move(reason)};
            }
            AURORA_LOG_WARN("tls", "candidate skipped: ", reason.message);
            exhausted = std::move(reason);
            continue;
        }
        auto backend = found->create(context, server_name);
        if (!backend) {
            if (designated || !is_skip_only(backend.error().code_enum)) {
                // 非跳过类错误说的是「这份配置不行」：口令错、PEM 读不开、版本界不合法。让它被下一枚
                // 后端「凑成功」等于把一次配置错误伪装成一次成功连接。
                return Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>{backend.error()};
            }
            AURORA_LOG_WARN("tls", "candidate skipped: ", wanted, " -> ", backend.error().message);
            exhausted = backend.error();
            continue;
        }
        return Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>{
            std::make_pair(std::move(backend.value()), found->id)};
    }
    return Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>{std::move(exhausted)};
}

}  // namespace aurora::net
