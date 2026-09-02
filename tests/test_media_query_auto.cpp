// media_query_auto_test.cpp — 覆盖 T8：Window::present_root 自动注入根 MediaQuery。
// 验证：无需手动包裹 MediaQueryProvider，根 widget 即能读取设备上下文；手动 Provider 仍按
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。
#include <memory>

#include "aurora/aurora.h"
#include "aurora/window/window.h"

#include "test_harness.h"

namespace au = aurora;
static auto size_eq(const au::Size &a, const au::Size &b, const float e = 1e-3f) -> bool {
    return near_f(a.width, b.width, e) && near_f(a.height, b.height, e);
}

static void test_auto_injection_root() {
    // T8：根 widget（无任何手动 Provider）应能经 media_query_of 读到自动注入的 MediaQuery。
    au::HeadlessOptions opts;
    opts.size = au::Size{ .width = 800.0f, .height = 600.0f };
    opts.title = "mq_auto";
    opts.png_path = "t8_mq_auto.png";
    auto win = std::move(au::create_window(opts).value());

    bool seen = false;
    au::MediaQuery cap{};
    auto host = au::LayoutBuilder{ [&](const au::BuildContext &c, const au::Constraints &cc) -> au::Node {
        (void)cc;
        if (const au::MediaQuery *mq = au::media_query_of(c)) {
            seen = true;
            cap = *mq;
        }
        return au::Node{ au::Text{ "auto" } };
    } };
    auto node = au::Node{ std::move(host) };
    (void)win->present_root(node);

    AURORA_TEST_CHECK_MSG(seen, "T8: root widget reads auto-injected MediaQuery (no manual Provider)");

    const au::MediaQuery expected = au::MediaQuery::from_surface(win->surface());
    if (seen) {
        AURORA_TEST_CHECK_MSG(near_f(cap.scale_factor, expected.scale_factor), "T8: scale_factor matches surface");
        AURORA_TEST_CHECK_MSG(size_eq(cap.screen_size, expected.screen_size), "T8: screen_size matches surface");
        AURORA_TEST_CHECK_MSG(cap.orientation == expected.orientation, "T8: orientation matches surface");
        AURORA_TEST_CHECK_MSG(cap.platform == expected.platform, "T8: platform matches surface");
        AURORA_TEST_CHECK_MSG(cap.device == expected.device, "T8: device matches surface");
        AURORA_TEST_CHECK_MSG(cap.prefer_reduced_motion == expected.prefer_reduced_motion,
                              "T8: reduced-motion matches surface");
    }
}

static void test_manual_provider_overrides() {
    // 手动 MediaQueryProvider 仍按「最近祖先优先」覆盖自动注入的默认值。
    au::HeadlessOptions opts;
    opts.size = au::Size{ .width = 800.0f, .height = 600.0f };
    opts.title = "mq_over";
    opts.png_path = "t8_mq_over.png";
    auto win = std::move(au::create_window(opts).value());

    au::MediaQuery custom;
    custom.scale_factor = 3.0f;
    custom.platform = au::PlatformKind::Web;

    bool seen = false;
    au::MediaQuery cap{};
    auto host = au::MediaQueryProvider{ custom, au::LayoutBuilder{ [&](const au::BuildContext &c,
                                                                       const au::Constraints &cc) -> au::Node {
                                            (void)cc;
                                            if (const au::MediaQuery *mq = au::media_query_of(c)) {
                                                seen = true;
                                                cap = *mq;
                                            }
                                            return au::Node{ au::Text{ "over" } };
                                        } } };
    auto node = au::Node{ std::move(host) };
    (void)win->present_root(node);

    AURORA_TEST_CHECK_MSG(seen, "T8: manual Provider still visible under auto-injection");
    AURORA_TEST_CHECK_MSG(seen && near_f(cap.scale_factor, 3.0f),
                          "T8: manual Provider overrides auto-injected scale_factor");
    AURORA_TEST_CHECK_MSG(seen && cap.platform == au::PlatformKind::Web, "T8: manual Provider overrides platform");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== media_query_auto_test (T8) ===\n");
    test_auto_injection_root();
    test_manual_provider_overrides();
}
