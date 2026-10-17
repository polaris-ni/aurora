/// 测试类型: unit
/// 目标单元: include/aurora/net/tls_backend.h + src/aurora/net/tls_backend.cpp
/// 测试说明: TLS 后端注册表与候选链解析的行为契约回归（全程零网络，且不需要任何真实 TLS 栈）。
///           注册纪律：空 id 与空工厂落 `general-invalid-argument`，重复 id 落 `net-option-conflict`
///           且**不覆盖**原实现（换实现必须先注销）；注销区分命中与未命中。链的形态：缺省链按
///           priority 降序、同优先级按注册次序；显式置链按给定次序**原样**使用而不重排；置链只校验
///           形态（空串项与重复项拒绝），不校验 id 是否已在案——「先配链、后加载插件」是合法启动顺序。
///           解析纪律是本文件的重心：只有「这一枚不可用」（未注册、`net-tls-engine-unavailable`、
///           `net-platform-unavailable`）才继续下一枚，且跳过必留 warning；配置类错误（如
///           `net-option-conflict`）终止整条链并原样上报，下一枚工厂一次都不被调用——让配置错误被
///           后一枚后端「凑成功」等于把失败伪装成连接。指名后端（`TlsContext::backend`）即用即不回落，
///           未注册的 id 单独指名是明确失败、排在链里是被跳过，两者的结局不同。空链在任何工厂之前
///           定论；全链被跳过时上报最后一枚跳过原因而不是凭空另造一句。能力位与注册表同源：清空即
///           false，注入桩后端即 true。工厂入参（口径与目标名）逐字透传，在案表面里的每一枚 id 都
///           能被指名解析出实例。
/// ⚠️ 平台与能力分支一律写在**用例体内**（registry_integrity 静态扫源码，被 #ifdef 摘掉的声明在
///    另一种构建里必然单边失踪）。本库的后端构建宏是 `PRIVATE` 作用域，测试 TU 看不到它们，因此
///    凡依赖「本腿编进了哪几条腿」的用例都从运行期表面 `available_tls_backends()` 出发，空表面时
///    报成缺项而不是失败；不依赖内置腿的用例（桩后端、链形态、解析纪律）在所有腿上实跑。
/// ⚠️ 注册表是进程级可写状态，而 runner 在同进程里先后跑全部用例。每条用例进出各经 `RegistryGuard`
///    存还原整表，桩后端与显式置链都不会泄漏给后续用例（包括 `utest_net_tls` 的真实握手）。

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/tls_backend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_tls_backend {
namespace {

using aurora::ErrorCode;
using aurora::Result;
using aurora::net::PeerCertificate;
using aurora::net::TlsBackend;
using aurora::net::TlsBackendCreator;
using aurora::net::TlsBackendRegistration;
using aurora::net::TlsContext;
using aurora::net::TlsDataState;
using aurora::net::detail::resolve_tls_backend;

using Resolved = Result<std::pair<std::unique_ptr<TlsBackend>, std::string>>;

/// @brief 桩后端的标识前缀：与内置 id 天然不撞，也让「本文件留下的注册」一眼可辨。
constexpr std::string_view AURORA_STUB_PREFIX{"utest-tls-"};

/// @brief 解析用例统一使用的目标名：桩不联网，但它必须收到一个 DNS 形态的名字。
constexpr std::string_view AURORA_SERVER_NAME{"registry.invalid"};

/// @brief 工厂调用的现场：次序、入参都记在这里，「下一枚没被叫」这条判据只能靠它证明。
struct CallLog {
    std::vector<std::string> order;  ///< 工厂被调次序（按后端 id 记录）
    std::string server_name;  ///< 最后一次收到的目标主机名
    std::optional<std::string> designated_backend;  ///< 最后一次入参里指名的后端
    std::vector<std::string> alpn;  ///< 最后一次入参里的 ALPN 宣告
    bool insecure = false;  ///< 最后一次入参里的危险开关
    int min_version = -1;  ///< 最后一次入参里的版本下界（枚举原值）

    /// @brief 记一次工厂调用。
    /// @param id 被调后端的 id。
    /// @param context 本次握手的口径。
    /// @param name 本次握手的目标名。
    auto note(std::string_view id, const TlsContext &context, std::string_view name) -> void {
        order.emplace_back(id);
        server_name = std::string{name};
        designated_backend = context.backend;
        insecure = context.allow_insecure_certificate;
        min_version = static_cast<int>(context.min_version);
        alpn = context.alpn_protocols;
    }

    /// @brief 某个 id 是否被叫过。
    /// @param id 后端 id。
    /// @return 出现在次序表里为 true。
    [[nodiscard]] auto called(std::string_view id) const -> bool {
        return std::find(order.begin(), order.end(), std::string{id}) != order.end();
    }

    /// @brief 某个 id 被叫的次数。
    /// @param id 后端 id。
    /// @return 出现次数。
    [[nodiscard]] auto count(std::string_view id) const -> std::size_t {
        return static_cast<std::size_t>(std::count(order.begin(), order.end(), std::string{id}));
    }
};

/// @brief inert 后端桩：七个虚函数全部给出确定的空转结局，实例身份经 `negotiated_alpn()` 交出。
///
/// 用 ALPN 回报标记而不是 `dynamic_cast` 认实例，是为了让「解析出的到底是哪一枚」这条判据只用公共
/// 契约就能表达——判据依赖向下转型会把桩的类型留在断言里，换桩即改断言。
class FakeBackend final : public TlsBackend {
  public:
    /// @param tag 实例标记（注册 id；经 `negotiated_alpn()` 交出）。
    explicit FakeBackend(std::string tag) : tag_(std::move(tag)) {}

    auto push_incoming(std::span<const char> bytes) -> void override { (void)bytes; }

    auto handshake(std::vector<char> &outgoing) -> Result<bool> override {
        (void)outgoing;
        return Result<bool>{false};
    }

    auto next_records(std::vector<char> &plaintext, std::vector<char> &outgoing) -> Result<TlsDataState> override {
        (void)plaintext;
        (void)outgoing;
        return Result<TlsDataState>{TlsDataState::NeedMore};
    }

    auto protect(std::span<const char> plaintext, std::vector<char> &outgoing) -> Result<void> override {
        (void)plaintext;
        (void)outgoing;
        return Result<void>{};
    }

    auto shutdown_records(std::vector<char> &outgoing) -> Result<void> override {
        (void)outgoing;
        return Result<void>{};
    }

    [[nodiscard]] auto peer_certificate() const -> Result<PeerCertificate> override {
        return Result<PeerCertificate>{
            make_error(ErrorCode::NetTlsHandshakeFailed, tag_ + ": the stub backend never handshakes")};
    }

    [[nodiscard]] auto negotiated_alpn() const -> std::optional<std::string> override { return tag_; }

  private:
    std::string tag_;  ///< 实例标记
};

/// @brief 造一枚总是成功的工厂。
/// @param id 后端 id（同时作为实例标记交出）。
/// @param log 调用现场；空指针 = 不计数。
/// @return 可直接放进 `TlsBackendRegistration::create` 的工厂。
auto ok_creator(std::string_view id, CallLog *log) -> TlsBackendCreator {
    return [name = std::string{id}, log](const TlsContext &context,
                                         std::string_view server_name) -> Result<std::unique_ptr<TlsBackend>> {
        if (log != nullptr) {
            log->note(name, context, server_name);
        }
        std::unique_ptr<TlsBackend> backend = std::make_unique<FakeBackend>(name);
        return backend;
    };
}

/// @brief 造一枚总是失败的工厂（错误码可控，用来分别触发「跳过」与「终止整条链」两类分支）。
/// @param id 后端 id（进错误文本，供次序判据）。
/// @param code 工厂要落的错误码。
/// @param log 调用现场；空指针 = 不计数。
/// @return 可直接放进 `TlsBackendRegistration::create` 的工厂。
auto failing_creator(std::string_view id, ErrorCode code, CallLog *log) -> TlsBackendCreator {
    return [name = std::string{id}, code, log](const TlsContext &context,
                                               std::string_view server_name) -> Result<std::unique_ptr<TlsBackend>> {
        if (log != nullptr) {
            log->note(name, context, server_name);
        }
        return Result<std::unique_ptr<TlsBackend>>{make_error(code, "stub backend " + name + " refuses this request")};
    };
}

/// @brief 注册表现场守卫：构造存整表，析构原样装回（含链与「链是否被显式置过」）。
class RegistryGuard {
  public:
    RegistryGuard() { aurora::net::detail::snapshot_tls_backend_registry(); }
    RegistryGuard(const RegistryGuard &) = delete;
    auto operator=(const RegistryGuard &) -> RegistryGuard & = delete;
    RegistryGuard(RegistryGuard &&) = delete;
    auto operator=(RegistryGuard &&) -> RegistryGuard & = delete;
    ~RegistryGuard() { aurora::net::detail::restore_tls_backend_registry(); }
};

/// @brief 登记一枚桩后端；注册失败即终止用例（造不出被测前提不该继续跑）。
/// @param id 后端 id。
/// @param priority 缺省链次序。
/// @param creator 工厂。
auto add_stub(std::string_view id, int priority, TlsBackendCreator creator) -> void {
    TlsBackendRegistration registration;
    registration.id = std::string{id};
    registration.priority = priority;
    registration.create = std::move(creator);
    const auto result = aurora::net::register_tls_backend(registration);
    AURORA_TEST_REQUIRE_MSG(result.ok(),
                            "stub registration failed for " + std::string{id} + ": " + result.error().message);
}

/// @brief 登记一枚总是成功的桩后端。
/// @param id 后端 id。
/// @param priority 缺省链次序。
/// @param log 调用现场（可空）。
auto add_ok(std::string_view id, int priority, CallLog *log = nullptr) -> void {
    add_stub(id, priority, ok_creator(id, log));
}

/// @brief 置全局缺省候选链；置链失败即终止用例。
/// @param chain 后端 id 序列。
auto use_chain(std::vector<std::string> chain) -> void {
    const auto result = aurora::net::set_default_tls_backends(std::move(chain));
    AURORA_TEST_REQUIRE(result.ok());
}

/// @brief 在案 id 里把桩剥掉，只留内置腿（跨腿可比的判据要用内置集合）。
/// @return 不以 `utest-tls-` 开头的 id 序列。
auto builtin_surface() -> std::vector<std::string> {
    std::vector<std::string> builtins;
    for (const auto &id : aurora::net::available_tls_backends()) {
        if (!id.starts_with(AURORA_STUB_PREFIX)) {
            builtins.push_back(id);
        }
    }
    return builtins;
}

/// @brief 表面里是否含某枚 id。
/// @param ids 待查表面。
/// @param id 目标 id。
/// @return 命中为 true。
auto surface_has(const std::vector<std::string> &ids, std::string_view id) -> bool {
    return std::find(ids.begin(), ids.end(), std::string{id}) != ids.end();
}

/// @brief 内置腿缺席时把用例报成缺项：这类用例判的是内置集合的次序与可构造性，没有内置腿便无对象。
/// @return 至少一枚内置腿时继续跑，否则终止本用例。
auto require_builtin() -> void {
    const auto builtins = builtin_surface();
    if (builtins.empty()) {
        AURORA_TEST_SKIP("this build carries no builtin tls backend, the case inspects the builtin surface only");
    }
}

/// @brief 跑一次候选链解析（不接触任何 I/O）。
/// @param context 安全口径。
/// @return 解析结局。
auto run_resolve(const TlsContext &context) -> Resolved { return resolve_tls_backend(context, AURORA_SERVER_NAME); }

/// @brief 解析结局的错误码 slug（成功态为空串）。
/// @param resolved 解析结局。
/// @return `error().code` 的副本。
auto error_code_of(const Resolved &resolved) -> std::string { return resolved ? std::string{} : resolved.error().code; }

/// @brief 解析出的实例标记（成功态）；空态交出空串。
/// @param resolved 解析结局。
/// @return 桩后端经 `negotiated_alpn()` 回报的标记。
auto tag_of(const Resolved &resolved) -> std::string {
    if (!resolved) {
        return {};
    }
    const auto tag = resolved.value().first->negotiated_alpn();
    return tag ? *tag : std::string{};
}

}  // namespace

// ---------------------------------------------------------------------------
// 注册与注销纪律
// ---------------------------------------------------------------------------

// 空 id 与空工厂都在进表之前被拒：注册表里留一条「没有名字的腿」或「造不出东西的腿」，代价要到某次
// 握手才暴露，而那时诊断已经无法指向注册点。
AURORA_TEST_CASE(registration_rejects_an_empty_id_or_an_empty_factory) {
    RegistryGuard guard;
    const auto before = aurora::net::available_tls_backends();

    TlsBackendRegistration no_id;
    no_id.priority = 10;
    no_id.create = ok_creator("utest-tls-no-id", nullptr);
    const auto rejected_id = aurora::net::register_tls_backend(no_id);
    AURORA_TEST_REQUIRE(bool(!rejected_id));
    AURORA_TEST_CHECK_STREQ(rejected_id.error().code, "general-invalid-argument");
    // 拒绝必须说得出「拒的是哪一项」：这枚码的表模板没有 detail 槽位，文本只能由构造点给全。
    AURORA_TEST_CHECK(rejected_id.error().message.find("non-empty id") != std::string::npos);

    TlsBackendRegistration no_factory;
    no_factory.id = std::string{AURORA_STUB_PREFIX} + "no-factory";
    const auto rejected_factory = aurora::net::register_tls_backend(no_factory);
    AURORA_TEST_REQUIRE(bool(!rejected_factory));
    AURORA_TEST_CHECK_STREQ(rejected_factory.error().code, "general-invalid-argument");
    AURORA_TEST_CHECK(rejected_factory.error().message.find("without a factory") != std::string::npos);

    AURORA_TEST_CHECK(aurora::net::available_tls_backends() == before);
}

// 重复 id 落冲突且**原实现仍在案**：静默替换别人的后端会让「谁在跑这条连接」变成查不出来的事实。
AURORA_TEST_CASE(duplicate_id_is_refused_without_replacing_the_original) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-original", 30, &log);

    TlsBackendRegistration again;
    again.id = "utest-tls-original";
    again.priority = 90;
    again.create = ok_creator("utest-tls-replacement", &log);
    const auto conflict = aurora::net::register_tls_backend(again);
    AURORA_TEST_REQUIRE(bool(!conflict));
    AURORA_TEST_CHECK_STREQ(conflict.error().code, "net-option-conflict");
    AURORA_TEST_CHECK(!conflict.error().retryable);
    AURORA_TEST_CHECK(conflict.error().message.find("unregister it first") != std::string::npos);

    use_chain({"utest-tls-original"});
    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(resolved.value().second, "utest-tls-original");
    AURORA_TEST_CHECK_STREQ(tag_of(resolved), "utest-tls-original");
    AURORA_TEST_CHECK_EQ(log.order.size(), std::size_t{1});
}

// 注销只回答「命中与否」：内置腿同样可注销（「这台机器我不想让这条腿参与」是调用方的合法决定），
// 未命中的 id 不该被造出一个错误码来打扰调用方。
AURORA_TEST_CASE(unregistration_reports_hit_and_miss) {
    RegistryGuard guard;
    add_ok("utest-tls-removable", 20);
    AURORA_TEST_CHECK(surface_has(aurora::net::available_tls_backends(), "utest-tls-removable"));

    AURORA_TEST_CHECK(aurora::net::unregister_tls_backend("utest-tls-removable"));
    AURORA_TEST_CHECK(!surface_has(aurora::net::available_tls_backends(), "utest-tls-removable"));
    AURORA_TEST_CHECK(!aurora::net::unregister_tls_backend("utest-tls-removable"));
    AURORA_TEST_CHECK(!aurora::net::unregister_tls_backend("utest-tls-never-registered"));
}

// 内置腿注销后既从表面消失，也让指名它的握手明确失败——「注销了却还在被选中」是最难查的形态。
AURORA_TEST_CASE(unregistering_a_builtin_id_removes_it_from_the_surface) {
    RegistryGuard guard;
    require_builtin();
    const auto builtins = builtin_surface();
    const auto &target = builtins.front();

    AURORA_TEST_CHECK(aurora::net::unregister_tls_backend(target));
    AURORA_TEST_CHECK(!surface_has(aurora::net::available_tls_backends(), target));

    TlsContext context;
    context.backend = target;
    const auto resolved = run_resolve(context);
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-tls-engine-unavailable");
    AURORA_TEST_CHECK(resolved.error().message.find("is not registered") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 候选链的形态
// ---------------------------------------------------------------------------

// 缺省链 = priority 降序、同 priority 保持注册次序。次序是「谁先被试」的唯一依据，排错即选错腿。
AURORA_TEST_CASE(default_chain_sorts_by_priority_then_registration_order) {
    RegistryGuard guard;
    add_ok("utest-tls-low", 5);
    add_ok("utest-tls-high-first", 70);
    add_ok("utest-tls-high-second", 70);

    const auto chain = aurora::net::default_tls_backends();
    const auto first = std::find(chain.begin(), chain.end(), std::string{"utest-tls-high-first"});
    const auto second = std::find(chain.begin(), chain.end(), std::string{"utest-tls-high-second"});
    const auto low = std::find(chain.begin(), chain.end(), std::string{"utest-tls-low"});
    AURORA_TEST_REQUIRE(first != chain.end());
    AURORA_TEST_REQUIRE(second != chain.end());
    AURORA_TEST_REQUIRE(low != chain.end());
    AURORA_TEST_CHECK(first < second);
    AURORA_TEST_CHECK(second < low);
}

// 显式置链按给定次序原样使用，不按 priority 重排：调用方把 priority 高的排在后面，就是在说「先试后面
// 那枚」，重排会把这个决定吃掉。
AURORA_TEST_CASE(explicit_chain_is_used_verbatim_not_resorted) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-priority-low", 5, &log);
    add_ok("utest-tls-priority-high", 95, &log);
    use_chain({"utest-tls-priority-low", "utest-tls-priority-high"});

    AURORA_TEST_CHECK(aurora::net::default_tls_backends().size() == std::size_t{2});
    AURORA_TEST_CHECK_STREQ(aurora::net::default_tls_backends().front(), "utest-tls-priority-low");

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(resolved.value().second, "utest-tls-priority-low");
    AURORA_TEST_CHECK_EQ(log.order.size(), std::size_t{1});
    AURORA_TEST_CHECK(!log.called("utest-tls-priority-high"));
}

// 置链只校验形态：空串项与重复项拒绝（重复项让「试两次同一枚」看起来像有意的重试策略），
// 但未知 id 照收——拒绝会把「先配链、后加载插件」变成不可用形态。
AURORA_TEST_CASE(chain_validation_checks_shape_only) {
    RegistryGuard guard;

    const auto empty_item = aurora::net::set_default_tls_backends({"utest-tls-a", ""});
    AURORA_TEST_REQUIRE(bool(!empty_item));
    AURORA_TEST_CHECK_STREQ(empty_item.error().code, "general-invalid-argument");
    AURORA_TEST_CHECK(empty_item.error().message.find("at index 1") != std::string::npos);

    const auto duplicated = aurora::net::set_default_tls_backends({"utest-tls-a", "utest-tls-b", "utest-tls-a"});
    AURORA_TEST_REQUIRE(bool(!duplicated));
    AURORA_TEST_CHECK_STREQ(duplicated.error().code, "general-invalid-argument");

    const auto unknown = aurora::net::set_default_tls_backends({"not-loaded-yet"});
    AURORA_TEST_CHECK(unknown.ok());
    AURORA_TEST_CHECK_EQ(aurora::net::default_tls_backends().size(), std::size_t{1});

    const auto empty_chain = aurora::net::set_default_tls_backends({});
    AURORA_TEST_CHECK(empty_chain.ok());
    AURORA_TEST_CHECK(aurora::net::default_tls_backends().empty());
}

// ---------------------------------------------------------------------------
// 解析纪律：回落只发生在握手开始之前
// ---------------------------------------------------------------------------

// 链里「这一枚不可用」与「这份配置不行」是两类事实：前者继续下一枚，后者终止整条链。
// 让配置错误被下一枚后端凑成功，等于把一次失败伪装成一次连接。
AURORA_TEST_CASE(unavailable_candidate_is_skipped_and_the_next_one_serves) {
    RegistryGuard guard;
    CallLog log;
    add_stub("utest-tls-refuses", 90, failing_creator("utest-tls-refuses", ErrorCode::NetPlatformUnavailable, &log));
    add_ok("utest-tls-serves", 10, &log);
    use_chain({"utest-tls-refuses", "utest-tls-serves"});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(resolved.value().second, "utest-tls-serves");
    AURORA_TEST_CHECK_EQ(log.count("utest-tls-refuses"), std::size_t{1});
    AURORA_TEST_CHECK_EQ(log.count("utest-tls-serves"), std::size_t{1});
}

// 未注册的 id 排在链里是被跳过（第三方插件可能还没加载），不是失败。
AURORA_TEST_CASE(unknown_id_inside_the_chain_is_skipped) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-after-gap", 10, &log);
    use_chain({"utest-tls-not-loaded", "utest-tls-after-gap"});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(resolved.value().second, "utest-tls-after-gap");
    AURORA_TEST_CHECK_EQ(log.order.size(), std::size_t{1});
}

// `engine-unavailable` 与 `platform-unavailable` 同属「这一枚不行」，都触发下一枚。
AURORA_TEST_CASE(engine_unavailable_code_also_continues_the_chain) {
    RegistryGuard guard;
    CallLog log;
    add_stub("utest-tls-no-engine", 90,
             failing_creator("utest-tls-no-engine", ErrorCode::NetTlsEngineUnavailable, &log));
    add_ok("utest-tls-next", 10, &log);
    use_chain({"utest-tls-no-engine", "utest-tls-next"});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(tag_of(resolved), "utest-tls-next");
}

// 非跳过类错误原样终止：下一枚工厂连一次都不该被叫到。
AURORA_TEST_CASE(configuration_error_terminates_the_whole_chain) {
    RegistryGuard guard;
    CallLog log;
    add_stub("utest-tls-bad-config", 90, failing_creator("utest-tls-bad-config", ErrorCode::NetOptionConflict, &log));
    add_ok("utest-tls-never-called", 10, &log);
    use_chain({"utest-tls-bad-config", "utest-tls-never-called"});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-option-conflict");
    AURORA_TEST_CHECK(!log.called("utest-tls-never-called"));
    AURORA_TEST_CHECK_EQ(log.order.size(), std::size_t{1});
}

// 指名即用：显式指定后端常常含合规口径（「这条连接必须走系统栈」），悄悄换成下一枚等于推翻调用方的决定。
// 这里不可用的那枚被指名，链里明明有可用后继也不得接手。
AURORA_TEST_CASE(designated_backend_never_falls_back) {
    RegistryGuard guard;
    CallLog log;
    add_stub("utest-tls-designated-bad", 90,
             failing_creator("utest-tls-designated-bad", ErrorCode::NetPlatformUnavailable, &log));
    add_ok("utest-tls-good", 10, &log);
    use_chain({"utest-tls-designated-bad", "utest-tls-good"});

    TlsContext context;
    context.backend = "utest-tls-designated-bad";
    const auto resolved = run_resolve(context);
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-platform-unavailable");
    AURORA_TEST_CHECK(!log.called("utest-tls-good"));
    AURORA_TEST_CHECK_EQ(log.count("utest-tls-designated-bad"), std::size_t{1});
}

// 指名的 id 不在案时也是明确失败，而且诊断要点名实际在案的 id——「写错名字」与「那条腿今天不在案」
// 在调用方那边是同一个动作，看得见的差异只有这句。
AURORA_TEST_CASE(designated_unknown_id_fails_without_consulting_the_chain) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-in-case", 10, &log);
    use_chain({"utest-tls-in-case"});

    TlsContext context;
    context.backend = "utest-tls-not-in-case";
    const auto resolved = run_resolve(context);
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-tls-engine-unavailable");
    AURORA_TEST_CHECK(resolved.error().message.find("utest-tls-not-in-case") != std::string::npos);
    AURORA_TEST_CHECK(resolved.error().message.find("utest-tls-in-case") != std::string::npos);
    AURORA_TEST_CHECK(!log.called("utest-tls-in-case"));
}

// 空链在任何工厂之前定论：这是调用方自己置出来的状态，教的是「去置链或注册」，不是「去改构建」。
AURORA_TEST_CASE(empty_chain_is_decided_before_any_factory_runs) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-idle", 10, &log);
    use_chain({});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-tls-engine-unavailable");
    AURORA_TEST_CHECK(resolved.error().message.find("empty") != std::string::npos);
    AURORA_TEST_CHECK(log.order.empty());
}

// 空链 + 指名后端时，指名优先：链为空不该让一次显式指定变成「无从选择」。
AURORA_TEST_CASE(designated_backend_overrides_an_empty_chain) {
    RegistryGuard guard;
    add_ok("utest-tls-designated", 10);
    use_chain({});

    TlsContext context;
    context.backend = "utest-tls-designated";
    const auto resolved = run_resolve(context);
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(resolved.value().second, "utest-tls-designated");
}

// 全链被跳过时上报最后一枚跳过原因：调用方要看见「试过了且都不行」，而不是凭空另造的一句。
AURORA_TEST_CASE(whole_chain_unavailable_reports_the_last_skip_reason) {
    RegistryGuard guard;
    CallLog log;
    add_stub("utest-tls-first-refuses", 90,
             failing_creator("utest-tls-first-refuses", ErrorCode::NetPlatformUnavailable, &log));
    add_stub("utest-tls-last-refuses", 50,
             failing_creator("utest-tls-last-refuses", ErrorCode::NetTlsEngineUnavailable, &log));
    use_chain({"utest-tls-first-refuses", "utest-tls-last-refuses"});

    const auto resolved = run_resolve(TlsContext{});
    AURORA_TEST_REQUIRE(bool(!resolved));
    AURORA_TEST_CHECK_STREQ(resolved.error().code, "net-tls-engine-unavailable");
    AURORA_TEST_CHECK(resolved.error().message.find("utest-tls-last-refuses refuses this request") !=
                      std::string::npos);
    AURORA_TEST_CHECK_EQ(log.order.size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// 能力位与入参透传
// ---------------------------------------------------------------------------

// 能力位由注册表回答：清空即 false，注入桩即 true。「有腿」与「调用真的能成功」因此同源。
AURORA_TEST_CASE(capability_bit_tracks_registry_emptiness) {
    RegistryGuard guard;
    const auto builtins = builtin_surface();
    for (const auto &id : builtins) {
        AURORA_TEST_CHECK(aurora::net::unregister_tls_backend(id));
    }
    // 内置腿之外的桩（若有）一并清空，判据要的只是「整表为空」这一个状态。
    for (const auto &id : aurora::net::available_tls_backends()) {
        AURORA_TEST_CHECK(aurora::net::unregister_tls_backend(id));
    }
    AURORA_TEST_CHECK(aurora::net::available_tls_backends().empty());
    AURORA_TEST_CHECK(!aurora::net::detail::tls_backend_available());

    add_ok("utest-tls-capability", 0);
    AURORA_TEST_CHECK(aurora::net::detail::tls_backend_available());
    AURORA_TEST_CHECK_EQ(aurora::net::available_tls_backends().size(), std::size_t{1});
}

// 工厂收到的是本次握手本身的口径与目标名，不是上一次的：会话层把 context 交给解析，解析原样转发。
AURORA_TEST_CASE(factory_receives_the_handshake_inputs_verbatim) {
    RegistryGuard guard;
    CallLog log;
    add_ok("utest-tls-inspector", 10, &log);
    use_chain({"utest-tls-inspector"});

    TlsContext context;
    context.allow_insecure_certificate = true;
    context.min_version = aurora::net::TlsVersion::Tls13;
    context.alpn_protocols = {"h3", "h2"};
    context.backend = "utest-tls-inspector";
    const auto resolved = run_resolve(context);
    AURORA_TEST_REQUIRE(bool(resolved));
    AURORA_TEST_CHECK_STREQ(log.server_name, "registry.invalid");
    AURORA_TEST_REQUIRE(bool(log.designated_backend.has_value()));
    AURORA_TEST_CHECK_STREQ(*log.designated_backend, "utest-tls-inspector");
    AURORA_TEST_CHECK(log.insecure);
    AURORA_TEST_CHECK_EQ(log.min_version, static_cast<int>(aurora::net::TlsVersion::Tls13));
    AURORA_TEST_CHECK_EQ(log.alpn.size(), std::size_t{2});
    AURORA_TEST_CHECK_STREQ(log.alpn.front(), "h3");
}

// 在册即能用：表面里每一枚内置腿都要能被指名解析出实例——这是「注册表非空」这句能力位承诺的现场证据。
AURORA_TEST_CASE(every_surface_id_is_designable_and_constructible) {
    RegistryGuard guard;
    require_builtin();
    for (const auto &id : builtin_surface()) {
        TlsContext context;
        context.backend = id;
        context.min_version = aurora::net::TlsVersion::Tls12;
        const auto resolved = run_resolve(context);
        if (!resolved) {
            // 指名不回落，因此这条腿对这个口径表态「表达不了」也是契约内的结局；码必须是那两枚之一，
            // 诊断文本必须点名后端，绝不能是「找不到后端」。
            AURORA_TEST_CHECK_MSG(resolved.error().code == "net-platform-unavailable" ||
                                      resolved.error().code == "net-tls-engine-unavailable",
                                  "backend " + id + " failed designation with " + error_code_of(resolved) + ": " +
                                      resolved.error().message);
            continue;
        }
        AURORA_TEST_CHECK_STREQ(resolved.value().second, id);
    }
}

// 缺省链第一次被读取时即等于在案表面的次序（未显式置链时二者同源），且副本改动不影响注册表。
AURORA_TEST_CASE(surface_and_default_chain_share_one_source_before_configuration) {
    RegistryGuard guard;
    add_ok("utest-tls-tail", -5);
    const auto surface = aurora::net::available_tls_backends();
    AURORA_TEST_CHECK(aurora::net::default_tls_backends() == surface);

    auto copy = surface;
    copy.clear();
    AURORA_TEST_CHECK(!aurora::net::available_tls_backends().empty());
    AURORA_TEST_CHECK_STREQ(surface.back(), "utest-tls-tail");
    AURORA_TEST_CHECK_EQ(aurora::net::available_tls_backends().back(), "utest-tls-tail");
}

}  // namespace aurora::test_cases::utest_net_tls_backend
