/// 测试类型: integration
/// 目标单元: include/aurora/window/window.h (detail::ContentInsetRoot / prepare_context)
/// 测试说明: 集成——CSD 安全区自动下沉：Window::present_root 将应用根挂到框架私有壳下，
///           Surface::content_inset 经 PaddingEdges 修饰自动下沉应用内容（绘制/命中同源）；
///           根注入 MediaQuery 调整为内容区口径（padding 归零、size 扣除内缩）；
///           inset 变化（模拟进出全屏归零）下一帧即生效，无需应用介入

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "aurora/aurora.h"
#include "aurora/environment/media_query.h"
#include "aurora/widget/layout_builder.h"
#include "aurora/widget/node.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_content_inset_sink {

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

/// 取窗口的 HeadlessSurface（测试 seam 入口）。
auto headless_surface(aurora::Window &win) -> aurora::HeadlessSurface & {
    return static_cast<aurora::HeadlessSurface &>(win.surface());
}

}  // namespace

AURORA_TEST_CASE(content_inset_sinks_root_and_adjusts_media_query) {
    auto win = make_window(headless_opts("inset_sink"));
    // 模拟 Wayland CSD 自绘标题栏预留：顶部 36dp。
    headless_surface(*win).set_content_inset(aurora::EdgeInsets{.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F});

    bool seen = false;
    aurora::MediaQuery cap{};
    auto host = aurora::LayoutBuilder{[&](const aurora::BuildContext &c, const aurora::Constraints &) -> aurora::Node {
        if (const aurora::MediaQuery *mq = aurora::media_query_of(c)) {
            seen = true;
            cap = *mq;
        }
        return aurora::Node{aurora::Text{"sink"}};
    }};
    aurora::Node node{std::move(host)};
    (void)win->present_root(node);

    AURORA_TEST_REQUIRE_MSG(seen, "root widget reads auto-injected MediaQuery");

    // 根注入口径 = 内容区：padding 归零（框架已消费），size 扣除顶部内缩。
    AURORA_TEST_CHECK_NEAR(cap.padding.top, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cap.padding.left, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(cap.size.width, 800.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(cap.size.height, 564.0F, 1e-3F);

    // 应用根整体下沉：绘制盒原点 = (0, 36)，尺寸 = 内容区（绘制/命中共用同一修饰平移）。
    const aurora::Rect bounds = node.widget().paint_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.origin.x, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(bounds.origin.y, 36.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 564.0F, 1e-3F);
}

AURORA_TEST_CASE(inset_reset_relayouts_next_frame) {
    // 模拟进出全屏：inset 归零后下一帧应用根回到原点、MediaQuery 恢复窗口口径——
    // 证明下沉是每帧布局期生效，而非建树时一次性偏移。
    auto win = make_window(headless_opts("inset_reset"));
    headless_surface(*win).set_content_inset(aurora::EdgeInsets{.left = 0.0F, .top = 36.0F, .right = 0.0F, .bottom = 0.0F});

    auto host = aurora::LayoutBuilder{[&](const aurora::BuildContext &, const aurora::Constraints &) -> aurora::Node {
        return aurora::Node{aurora::Text{"reset"}};
    }};
    aurora::Node node{std::move(host)};
    (void)win->present_root(node);
    AURORA_TEST_CHECK_NEAR(node.widget().paint_bounds().origin.y, 36.0F, 1e-3F);

    headless_surface(*win).set_content_inset(aurora::EdgeInsets{});
    bool seen = false;
    aurora::MediaQuery cap{};
    // 换一个会重新读 MediaQuery 的树触发整帧重排（inset 变化经 force_full_redraw 驱动）。
    auto host2 =
        aurora::LayoutBuilder{[&](const aurora::BuildContext &c, const aurora::Constraints &) -> aurora::Node {
            if (const aurora::MediaQuery *mq = aurora::media_query_of(c)) {
                seen = true;
                cap = *mq;
            }
            return aurora::Node{aurora::Text{"reset2"}};
        }};
    aurora::Node node2{std::move(host2)};
    (void)win->present_root(node2);

    AURORA_TEST_REQUIRE_MSG(seen, "MediaQuery re-read after inset reset");
    AURORA_TEST_CHECK_NEAR(cap.size.height, 600.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(node2.widget().paint_bounds().origin.y, 0.0F, 1e-3F);
}

AURORA_TEST_CASE(zero_inset_keeps_legacy_geometry) {
    // inset 恒零（Win32/X11 原生非客户区）：壳修饰链为空，应用根绘制盒与历史行为逐位一致。
    auto win = make_window(headless_opts("inset_zero"));

    auto host = aurora::LayoutBuilder{[&](const aurora::BuildContext &, const aurora::Constraints &) -> aurora::Node {
        return aurora::Node{aurora::Text{"zero"}};
    }};
    aurora::Node node{std::move(host)};
    (void)win->present_root(node);

    const aurora::Rect bounds = node.widget().paint_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.origin.x, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(bounds.origin.y, 0.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(bounds.size.width, 800.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 600.0F, 1e-3F);
}

}  // namespace aurora::test_cases::itest_content_inset_sink
