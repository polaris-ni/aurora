/// 测试类型: unit
/// 目标单元: include/aurora/app/os_hotkey.h
/// 测试说明: 覆盖句柄值语义、注册表 API 形状（无效/未知句柄 remove、空表排空与清空）、
/// 不支持平台上的降级路径（enabled()==false 且 add() 返回 OsHotkeyRegisterFailed）、
/// 同一组合重复注册被拒、以及无窗口/无后端环境下的构造与析构不崩溃。
/// 全程**不注册真实系统热键**：受支持的平台上改走进程内 inert 测试后端
/// （AURORA_ENABLE_DEBUG + AURORA_ENABLE_TEST_HOOKS 双宏齐备时可用），CI 上不干扰系统。

#include <cstddef>
#include <cstdint>
#include <string>

#include "aurora/app/os_hotkey.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_os_hotkey {

namespace {

/// @brief 缺省的测试组合（Ctrl+Shift+K）。
[[nodiscard]] auto sample_combo() -> KeyCombo {
    return KeyCombo{ModifierKey::Control | ModifierKey::Shift, KeyCode::K};
}

/// @brief 安装 inert 后端（不触碰 OS）；双宏未齐备时跳过当前用例。
/// @param supported 模拟的平台支持位。
auto require_inert_backend(bool supported) -> void {
    if (!OsHotkeyRegistry::install_test_backend(supported)) {
        AURORA_TEST_SKIP(
            "os hotkey inert test backend unavailable "
            "(AURORA_ENABLE_DEBUG/AURORA_ENABLE_TEST_HOOKS not both set)");
    }
}

/// @brief 让「平台不支持」成为当前事实：优先装 inert 后端；装不上时仅当本平台本就不支持才继续。
auto require_unsupported_platform() -> void {
    if (OsHotkeyRegistry::install_test_backend(false)) {
        return;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_SKIP("cannot simulate an unsupported platform without the inert test backend");
#endif
}

}  // namespace

AURORA_TEST_CASE(default_handle_is_invalid) {
    const OsHotkeyHandle invalid{};
    AURORA_TEST_CHECK_EQ(invalid.id, 0U);
    AURORA_TEST_CHECK_TRUE(invalid == OsHotkeyHandle{});
    AURORA_TEST_CHECK_FALSE(invalid != OsHotkeyHandle{});
}

AURORA_TEST_CASE(handle_equality_follows_id) {
    const OsHotkeyHandle a{1U};
    const OsHotkeyHandle b{1U};
    const OsHotkeyHandle c{2U};
    AURORA_TEST_CHECK_TRUE(a == b);
    AURORA_TEST_CHECK_FALSE(a == c);
    AURORA_TEST_CHECK_TRUE(a != c);
}

AURORA_TEST_CASE(remove_unknown_handle_returns_false) {
    OsHotkeyRegistry registry;
    AURORA_TEST_CHECK_FALSE(registry.remove(OsHotkeyHandle{}));  // 无效句柄
    AURORA_TEST_CHECK_FALSE(registry.remove(OsHotkeyHandle{0xFFFFFFFFU}));  // 从未发放过的 ID
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});
}

AURORA_TEST_CASE(fresh_registry_is_empty_and_drains_nothing) {
    OsHotkeyRegistry registry;
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});
    AURORA_TEST_CHECK_EQ(registry.drain_pending(), std::size_t{0});
    registry.clear();  // 空表清空不得崩溃
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});
}

AURORA_TEST_CASE(unsupported_platform_degrades_to_register_error) {
    require_unsupported_platform();

    OsHotkeyRegistry registry;
    AURORA_TEST_CHECK_FALSE(registry.enabled());
    const auto res = registry.add(sample_combo(), []() -> void {});
    AURORA_TEST_REQUIRE_FALSE(res.ok());
    AURORA_TEST_CHECK(res.error().code_enum == ErrorCode::OsHotkeyRegisterFailed);
    AURORA_TEST_CHECK_FALSE(res.error().message.empty());  // detail 已渲染进 message
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(registry.remove(OsHotkeyHandle{1U}));

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(unsupported_platform_reports_disabled_before_any_registration) {
    require_unsupported_platform();

    // 只读查询也不得触碰 OS：enabled() 的返回值只由平台支持位决定。
    const OsHotkeyRegistry registry;
    AURORA_TEST_CHECK_FALSE(registry.enabled());

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(combination_without_primary_key_is_rejected) {
    require_inert_backend(true);

    OsHotkeyRegistry registry;
    AURORA_TEST_REQUIRE_TRUE(registry.enabled());
    const auto res = registry.add(KeyCombo{ModifierKey::Control, KeyCode::Unknown}, []() -> void {});
    AURORA_TEST_REQUIRE_FALSE(res.ok());
    AURORA_TEST_CHECK(res.error().code_enum == ErrorCode::OsHotkeyRegisterFailed);
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(duplicate_combination_is_rejected) {
    require_inert_backend(true);

    OsHotkeyRegistry registry;
    const auto first = registry.add(sample_combo(), []() -> void {});
    AURORA_TEST_REQUIRE_TRUE(first.ok());
    AURORA_TEST_CHECK_NE(first.value().id, 0U);
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{1});

    // 同一组合二次注册必须显式失败（而不是静默顶掉第一条）。
    const auto second = registry.add(sample_combo(), []() -> void {});
    AURORA_TEST_REQUIRE_FALSE(second.ok());
    AURORA_TEST_CHECK(second.error().code_enum == ErrorCode::OsHotkeyRegisterFailed);
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{1});

    // 修饰键不同即不同组合，可以再注册。
    const auto shifted = registry.add(KeyCombo{ModifierKey::Control, KeyCode::K}, []() -> void {});
    AURORA_TEST_REQUIRE_TRUE(shifted.ok());
    AURORA_TEST_CHECK_TRUE(shifted.value() != first.value());
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{2});

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(remove_registered_handle_succeeds_once) {
    require_inert_backend(true);

    OsHotkeyRegistry registry;
    const auto handle = registry.add(sample_combo(), []() -> void {});
    AURORA_TEST_REQUIRE_TRUE(handle.ok());
    AURORA_TEST_CHECK_TRUE(registry.remove(handle.value()));
    AURORA_TEST_CHECK_EQ(registry.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(registry.remove(handle.value()));  // 二次注销：已不存在

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(drain_without_hits_executes_nothing) {
    require_inert_backend(true);

    OsHotkeyRegistry registry;
    bool fired = false;
    const auto handle = registry.add(sample_combo(), [&fired]() -> void { fired = true; });
    AURORA_TEST_REQUIRE_TRUE(handle.ok());
    // inert 后端下没有消息泵投递命中：排空必须是 0，且不得凭空执行动作。
    AURORA_TEST_CHECK_EQ(registry.drain_pending(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(fired);

    (void)OsHotkeyRegistry::remove_test_backend();
}

AURORA_TEST_CASE(registry_without_window_backend_does_not_crash) {
    // 走 inert 后端（supported=false）：既不触碰 OS 热键接口，也不依赖平台支持位，
    // 从而能在任何目标上验证「反复构造 / 查询 / 注册失败 / 清空 / 析构」不崩溃、不抛。
    require_inert_backend(false);

    for (int i = 0; i < 3; ++i) {
        OsHotkeyRegistry registry;
        (void)registry.enabled();
        (void)registry.count();
        (void)registry.drain_pending();
        const auto res = registry.add(sample_combo(), []() -> void {});
        (void)res.ok();
        registry.clear();
    }

    (void)OsHotkeyRegistry::remove_test_backend();
}

}  // namespace aurora::test_cases::utest_os_hotkey
