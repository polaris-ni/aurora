// media_query_test.cpp — 覆盖 MediaQuery 完整维度（结构/默认值/from_surface/方向派生/
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

#include "aurora/aurora.h"
#include "aurora/core/platform.h"

#include "test_harness.h"

namespace au = aurora;
static auto size_eq(const au::Size &a, const au::Size &b, const float e = 1e-3f) -> bool {
    return near_f(a.width, b.width, e) && near_f(a.height, b.height, e);
}
static auto insets_eq(const au::EdgeInsets &a, const au::EdgeInsets &b, const float e = 1e-3f) -> bool {
    return near_f(a.left, b.left, e) && near_f(a.top, b.top, e) && near_f(a.right, b.right, e) &&
           near_f(a.bottom, b.bottom, e);
}

static void test_defaults() {
    const au::MediaQuery mq;
    AURORA_TEST_CHECK_MSG(size_eq(mq.size, au::Size{}), "MediaQuery: default size");
    AURORA_TEST_CHECK_MSG(near_f(mq.scale_factor, 1.0f), "MediaQuery: default scale_factor=1");
    AURORA_TEST_CHECK_MSG(near_f(mq.text_scale_factor, 1.0f), "MediaQuery: default text_scale_factor=1");
    AURORA_TEST_CHECK_MSG(mq.orientation == au::ScreenOrientation::Portrait,
                          "MediaQuery: default orientation=Portrait");
    AURORA_TEST_CHECK_MSG(size_eq(mq.screen_size, au::Size{}), "MediaQuery: default screen_size");
    AURORA_TEST_CHECK_MSG(mq.platform == au::PlatformKind::Unknown, "MediaQuery: default platform=Unknown");
    AURORA_TEST_CHECK_MSG(mq.device == au::DeviceKind::Unknown, "MediaQuery: default device=Unknown");
    AURORA_TEST_CHECK_MSG(insets_eq(mq.padding, au::EdgeInsets{}), "MediaQuery: default padding");
    AURORA_TEST_CHECK_MSG(mq.prefer_reduced_motion == false, "MediaQuery: default prefer_reduced_motion=false");
}

static void test_from_surface() {
    au::HeadlessSurface surf;
    (void)surf.begin_frame(800, 600);
    const auto mq = au::MediaQuery::from_surface(surf);
    // Headless 后端缩放因子恒为 1.0。
    AURORA_TEST_CHECK_MSG(near_f(mq.scale_factor, 1.0f), "from_surface: scale_factor=1 (headless surface)");
    // 窗口逻辑尺寸取自 Surface。
    AURORA_TEST_CHECK_MSG(near_f(mq.size.width, 800.0f) && near_f(mq.size.height, 600.0f),
                          "from_surface: size = surface logical size");
    // 方向由 screen_size 派生（与真实/模拟屏幕尺寸一致）。
    const bool expect_landscape = (mq.screen_size.width >= mq.screen_size.height);
    AURORA_TEST_CHECK_MSG(expect_landscape ? (mq.orientation == au::ScreenOrientation::Landscape)
                                           : (mq.orientation == au::ScreenOrientation::Portrait),
                          "from_surface: orientation derived from screen_size");
    AURORA_TEST_CHECK_MSG(mq.screen_size.width > 0.0f && mq.screen_size.height > 0.0f,
                          "from_surface: screen_size positive");

#ifdef AURORA_PLATFORM_WINDOWS
    // Win32 后端下取真实平台/设备，真实屏幕逻辑尺寸。
    AURORA_TEST_CHECK_MSG(mq.platform == au::PlatformKind::Windows, "from_surface(Win32): platform=Windows");
    AURORA_TEST_CHECK_MSG(mq.device == au::DeviceKind::Desktop, "from_surface(Win32): device=Desktop");
#else
    // 非 Windows 后端回退 Unknown（headless）。
    AURORA_TEST_CHECK_MSG(mq.platform == au::PlatformKind::Unknown, "from_surface(headless): platform=Unknown");
    AURORA_TEST_CHECK_MSG(mq.device == au::DeviceKind::Unknown, "from_surface(headless): device=Unknown");
    AURORA_TEST_CHECK_MSG(size_eq(mq.screen_size, mq.size), "from_surface(headless): screen_size=size");
#endif
}

static void test_provider_readback() {
    // 注意：Environment 注入链仅对 Provider 的「后代」可见（Provider 在子 ctx 上设置 env）。
    // 故在 Provider 的子节点（这里用 LayoutBuilder）内读取，而非在根 ctx 上读。
    au::MediaQuery injected;
    injected.scale_factor = 2.5f;
    injected.platform = au::PlatformKind::Windows;

    au::MediaQuery captured{};
    bool seen = false;
    auto host = au::MediaQueryProvider{ injected, au::LayoutBuilder{ [&](const au::BuildContext &c,
                                                                         const au::Constraints &cc) -> au::Node {
                                            (void)cc;
                                            const au::MediaQuery *mq = au::media_query_of(c);
                                            if (mq != nullptr) {
                                                seen = true;
                                                captured = *mq;
                                            }
                                            return au::Node{ au::Text{ "leaf" } };
                                        } } };
    auto node = au::Node{ std::move(host) };
    const au::BuildContext ctx;
    node.widget().mount(ctx);
    au::Constraints c;
    c.min = au::Size{ .width = 0.0f, .height = 0.0f };
    c.max = au::Size{ .width = 1000.0f, .height = 1000.0f };
    node.widget().layout(c, ctx);

    AURORA_TEST_CHECK_MSG(seen, "media_query_of: finds ancestor Provider");
    AURORA_TEST_CHECK_MSG(seen && near_f(captured.scale_factor, 2.5f), "media_query_of: scale_factor=2.5");
    AURORA_TEST_CHECK_MSG(seen && captured.platform == au::PlatformKind::Windows, "media_query_of: platform=Windows");

    // 根 ctx（无 Provider 祖先）：of 回默认。
    AURORA_TEST_CHECK_MSG(near_f(au::MediaQuery::of(ctx).scale_factor, 1.0f),
                          "MediaQuery::of: default at root (no Provider ancestor)");

    // 完全无 Provider：media_query_of 为 nullptr，of 回默认。
    const au::BuildContext empty;
    AURORA_TEST_CHECK_MSG(au::media_query_of(empty) == nullptr, "media_query_of: nullptr when no Provider");
    AURORA_TEST_CHECK_MSG(near_f(au::MediaQuery::of(empty).scale_factor, 1.0f),
                          "MediaQuery::of: default when no Provider");
}

static void test_layout_builder() {
    // 经 Provider 读回 MediaQuery（约束驱动构建）。
    bool saw_provider = false;
    au::MediaQuery injected;
    injected.scale_factor = 1.5f;
    auto host = au::MediaQueryProvider{ injected, au::LayoutBuilder{ [&](const au::BuildContext &c,
                                                                         const au::Constraints &cc) -> au::Node {
                                            (void)cc;
                                            const au::MediaQuery *mq = au::media_query_of(c);
                                            saw_provider = (mq != nullptr && near_f(mq->scale_factor, 1.5f));
                                            return au::Node{ au::Text{ "branch" } };
                                        } } };
    auto node = au::Node{ std::move(host) };
    const au::BuildContext ctx;
    node.widget().mount(ctx);

    au::Constraints c;
    c.min = au::Size{ .width = 0.0f, .height = 0.0f };
    c.max = au::Size{ .width = 1000.0f, .height = 1000.0f };
    node.widget().layout(c, ctx); // 首次构建
    AURORA_TEST_CHECK_MSG(saw_provider, "LayoutBuilder: builder reads MediaQuery from Provider");

    // 同样约束再次布局：不应再次重建（builder 仅应被调用一次）。
    int calls = 0;
    auto host2 = au::MediaQueryProvider{ injected, au::LayoutBuilder{ [&](const au::BuildContext &context,
                                                                          const au::Constraints &cc) -> au::Node {
                                             (void)context;
                                             (void)cc;
                                             ++calls;
                                             return au::Node{ au::Text{ "branch" } };
                                         } } };
    auto node2 = au::Node{ std::move(host2) };
    const au::BuildContext ctx2;
    node2.widget().mount(ctx2);
    node2.widget().layout(c, ctx2);
    node2.widget().layout(c, ctx2); // 约束相同 → 不重建
    AURORA_TEST_CHECK_MSG(calls == 1, "LayoutBuilder: caches child when constraints unchanged");

    // 约束变化 → 重建。
    au::Constraints c2;
    c2.min = au::Size{ .width = 0.0f, .height = 0.0f };
    c2.max = au::Size{ .width = 320.0f, .height = 480.0f };
    node2.widget().layout(c2, ctx2);
    AURORA_TEST_CHECK_MSG(calls == 2, "LayoutBuilder: rebuilds when constraints change");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== media_query_test ===\n");
    test_defaults();
    test_from_surface();
    test_provider_readback();
    test_layout_builder();
}
