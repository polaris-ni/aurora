/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/atspi_bridge.cpp
/// 测试说明: AT-SPI2 桥的宿主侧生命周期面 —— `NO_AT_BRIDGE=1` 显式免提（全平台可跑）；
///           真机用例（`AURORA_LIVE_ATSPI=1` 选择加入）走完整 dlopen + 总线连接 +
///           Socket.Embed 握手 + watch fd 暴露 + 拆除不崩（外部客户端视角由
///           tools/verify 探针把关）

#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/window/detail/atspi_bridge.h"
#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_LINUX) && (defined(AURORA_BACKEND_X11) || defined(AURORA_BACKEND_WAYLAND))
#define AURORA_ATSPI_BRIDGE_AVAILABLE 1
#include "aurora/widget/widget.h"
#else
#define AURORA_ATSPI_BRIDGE_AVAILABLE 0
#endif

namespace aurora::test_cases::utest_atspi_bridge {

#if AURORA_ATSPI_BRIDGE_AVAILABLE

namespace {

/// @brief 最小可投影根：LeafWidget + 显式语义（is_control）让快照有活节点。
class ProbeRoot final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char *override { return "ProbeRoot"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        return Size{.width = 200.0F, .height = 100.0F};
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

[[nodiscard]] auto probe_env() -> detail::AtspiEnv {
    detail::AtspiEnv env;
    env.app_name = "aurora-utest";
    env.window_title = "utest_atspi_bridge";
    env.to_window_px = [](const Rect &r) -> detail::AtspiRectI {
        return detail::AtspiRectI{.x = static_cast<std::int32_t>(r.origin.x),
                                  .y = static_cast<std::int32_t>(r.origin.y),
                                  .width = static_cast<std::int32_t>(r.size.width),
                                  .height = static_cast<std::int32_t>(r.size.height)};
    };
    env.to_screen_px = env.to_window_px;
    env.perform = [](Widget *w, const AccessibilityActionRequest &req) -> bool {
        return w != nullptr && w->perform_accessibility_action(req);
    };
    return env;
}

}  // namespace

AURORA_TEST_CASE(no_at_bridge_env_forces_degradation) {
#if defined(AURORA_PLATFORM_WINDOWS)
    (void)_putenv_s("NO_AT_BRIDGE", "1");
#else
    (void)::setenv("NO_AT_BRIDGE", "1", 1);
#endif
    // 显式免提（GNOME 惯例）：不碰 libdbus/总线，create 恒 nullptr。
    AURORA_TEST_CHECK_NULL(detail::AtspiBridge::create(probe_env()).get());
#if defined(AURORA_PLATFORM_WINDOWS)
    (void)_putenv_s("NO_AT_BRIDGE", "");
#else
    (void)::unsetenv("NO_AT_BRIDGE");
#endif
}

AURORA_TEST_CASE(live_embed_handshake_and_teardown) {
    const char *opt_in = std::getenv("AURORA_LIVE_ATSPI");
    if (opt_in == nullptr || opt_in[0] == '\0' || std::string_view{opt_in} == "0") {
        AURORA_TEST_SKIP("需显式置 AURORA_LIVE_ATSPI=1：本用例连接真实 a11y 总线并完成 Embed");
        return;
    }
    auto bridge = detail::AtspiBridge::create(probe_env());
    AURORA_TEST_REQUIRE_NOT_NULL(bridge.get());  // 注册表可达性由探针负责；这里必须成功
    AURORA_TEST_CHECK_TRUE(bridge->is_active());
    // Embed 成功 ⇒ 传输 watch fd 至少一枚（unix socket）。
    const std::vector<detail::AtspiBridge::WatchFd> watches = bridge->poll_watches();
    AURORA_TEST_CHECK_TRUE(!watches.empty());
    bool all_unix = true;
    for (const auto &w : watches) {
        all_unix = all_unix && w.fd >= 0;
    }
    AURORA_TEST_CHECK_TRUE(all_unix);
    // 注入语义树根 → pump 消化在途消息（Properties.Set("Application","Id") 回填路径）。
    ProbeRoot root;
    bridge->set_root(&root);
    bridge->pump();
    bridge->set_window_title("live");
    bridge->set_window_origin(10, 20);
    bridge->pump();
    bridge->sync_if_dirty();
    // 拆除（析构 = deactivate + 关连接）不得崩溃；析构后总线引用即失效（探针在外部验证）。
    bridge.reset();
    AURORA_TEST_CHECK_TRUE(true);
}

#else

AURORA_TEST_CASE(no_at_bridge_env_forces_degradation) {
    AURORA_TEST_SKIP("AURORA_BACKEND_X11/WAYLAND 未开启（非 Linux 平台），桥 TU 整体被宏剔除");
}

AURORA_TEST_CASE(live_embed_handshake_and_teardown) {
    AURORA_TEST_SKIP("AURORA_BACKEND_X11/WAYLAND 未开启（非 Linux 平台），桥 TU 整体被宏剔除");
}

#endif

}  // namespace aurora::test_cases::utest_atspi_bridge
