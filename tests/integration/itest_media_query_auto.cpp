/// 测试类型: integration
/// 目标单元: include/aurora/environment/media_query.h
/// 测试说明: T8 集成——Window::present_root 自动注入根 MediaQuery：根 widget（无手动 Provider）
///           经 media_query_of 读到与 from_surface(surface) 一致的设备上下文；
///           手动 MediaQueryProvider 仍按「最近祖先优先」覆盖自动注入默认值

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "aurora/aurora.h"
#include "aurora/environment/media_query.h"
#include "aurora/widget/layout_builder.h"
#include "aurora/widget/node.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/text.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_media_query_auto {

namespace {

/// Headless 窗口选项：固定 800x600，PNG 输出到当前用例唯一临时目录。
auto headless_opts(const std::string &name) -> aurora::HeadlessOptions {
    aurora::HeadlessOptions opts;
    opts.size = aurora::Size{.width = 800.0F, .height = 600.0F};
    opts.title = name;
    opts.png_path = (std::filesystem::path(aurora::testing::isolation::temp_dir()) / (name + ".png")).string();
    return opts;
}

/// 创建 Headless 窗口（失败为致命断言）。
auto make_window(const aurora::HeadlessOptions &opts) -> std::unique_ptr<aurora::Window> {
    auto created = aurora::create_window(opts);
    AURORA_TEST_REQUIRE_MSG(created.ok(), "create_window(HeadlessOptions) failed");
    return std::move(created.value());
}

}  // namespace

AURORA_TEST_CASE(root_widget_reads_auto_injected_media_query) {
    // T8：根 widget（无任何手动 Provider）应能经 media_query_of 读到自动注入的 MediaQuery。
    auto win = make_window(headless_opts("mq_auto"));

    bool seen = false;
    aurora::MediaQuery cap{};
    auto host = aurora::LayoutBuilder{[&](const aurora::BuildContext &c, const aurora::Constraints &) -> aurora::Node {
        if (const aurora::MediaQuery *mq = aurora::media_query_of(c)) {
            seen = true;
            cap = *mq;
        }
        return aurora::Node{aurora::Text{"auto"}};
    }};
    aurora::Node node{std::move(host)};
    (void)win->present_root(node);

    AURORA_TEST_REQUIRE_MSG(seen, "T8: root widget reads auto-injected MediaQuery (no manual Provider)");

    const aurora::MediaQuery expected = aurora::MediaQuery::from_surface(win->surface());
    AURORA_TEST_CHECK_NEAR(cap.scale_factor, expected.scale_factor, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cap.screen_size.width, expected.screen_size.width, 1e-3F);
    AURORA_TEST_CHECK_NEAR(cap.screen_size.height, expected.screen_size.height, 1e-3F);
    AURORA_TEST_CHECK(cap.orientation == expected.orientation);
    AURORA_TEST_CHECK(cap.platform == expected.platform);
    AURORA_TEST_CHECK(cap.device == expected.device);
    AURORA_TEST_CHECK_EQ(cap.prefer_reduced_motion, expected.prefer_reduced_motion);
}

AURORA_TEST_CASE(manual_provider_overrides_auto_injection) {
    // 手动 MediaQueryProvider 仍按「最近祖先优先」覆盖自动注入的默认值。
    auto win = make_window(headless_opts("mq_over"));

    aurora::MediaQuery custom;
    custom.scale_factor = 3.0F;
    custom.platform = aurora::PlatformKind::Web;

    bool seen = false;
    aurora::MediaQuery cap{};
    auto host = aurora::MediaQueryProvider{
        custom, aurora::LayoutBuilder{
                    [&](const aurora::BuildContext &c, const aurora::Constraints &) -> aurora::Node {
                        if (const aurora::MediaQuery *mq = aurora::media_query_of(c)) {
                            seen = true;
                            cap = *mq;
                        }
                        return aurora::Node{aurora::Text{"over"}};
                    }}};
    aurora::Node node{std::move(host)};
    (void)win->present_root(node);

    AURORA_TEST_REQUIRE_MSG(seen, "T8: manual Provider still visible under auto-injection");
    AURORA_TEST_CHECK_NEAR(cap.scale_factor, 3.0F, 1e-4F);
    AURORA_TEST_CHECK(cap.platform == aurora::PlatformKind::Web);
}

}  // namespace aurora::test_cases::itest_media_query_auto
