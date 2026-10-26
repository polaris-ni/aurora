/// 测试类型: integration
/// 目标单元: include/aurora/widget/icon_widget.h、include/aurora/theming/theme_scope.h
/// 测试说明: 以主题作用域包裹同一棵图标树，验证换肤重绘（同一结构在明/暗两套主题下复用相同的
///           DisplayList 命令数、渲染像素随主题默认色变化），并以明/暗两套 golden 基线锁定
///           图廊场景的绘制结果。资产全部为内存 SVG，不依赖任何图标字体。
/// 覆盖说明: 四态取色与默认取色的精确色值由单测 utest_icon.cpp 覆盖；本文件只验证集成层的
///           换肤重绘与命令结构复用，避免与单测口径重叠。

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/icon/icon.h"
#include "aurora/render/display_list.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/icon_widget.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::itest_icon_theme {

namespace {

namespace golden = aurora::testing::golden;

constexpr int AURORA_CANVAS_W = 320;
constexpr int AURORA_CANVAS_H = 200;

/// 以只含 `<rect>` 的最小 SVG 描述一个图标（内置 SVG 子集光栅器可处理）。
/// @param size 画布边长。
/// @param rects 若干 [x, y, w, h] 矩形。
/// @return SVG 源字节。
auto svg_from_rects(int size, const std::vector<std::array<int, 4>> &rects) -> std::vector<std::uint8_t> {
    std::string s = R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" + std::to_string(size) + R"(" height=")" +
                    std::to_string(size) + R"(">)";
    for (const auto &r : rects) {
        s += R"(<rect x=")" + std::to_string(r[0]) + R"(" y=")" + std::to_string(r[1]) + R"(" width=")" +
             std::to_string(r[2]) + R"(" height=")" + std::to_string(r[3]) + R"(" fill="#FFFFFF"/>)";
    }
    s += "</svg>";
    return {s.begin(), s.end()};
}

/// 重置并注册固定的示例图标集（进程级注册表，逐用例重置以保证确定性）。
auto ensure_icons() -> void {
    constexpr int box = 32;
    icon::IconRegistry::clear();
    icon::IconRegistry::register_icon("square", icon::IconData::svg(svg_from_rects(box, {{4, 4, 24, 24}}), "square"));
    icon::IconRegistry::register_icon(
        "plus", icon::IconData::svg(svg_from_rects(box, {{12, 4, 8, 24}, {4, 12, 24, 8}}), "plus"));
    icon::IconRegistry::register_icon("minus", icon::IconData::svg(svg_from_rects(box, {{6, 14, 20, 4}}), "minus"));
    icon::IconRegistry::register_icon(
        "frame", icon::IconData::svg(
                     svg_from_rects(box, {{4, 4, 24, 3}, {4, 25, 24, 3}, {4, 4, 3, 24}, {25, 4, 3, 24}}), "frame"));
}

/// 确定性图廊树：混合显式着色（跨主题不变）与跟随主题默认色（跨主题变化）的图标，
/// 以及常态 / 禁用态 IconButton。不含文本，故不牵涉字体，golden 仅由图标像素决定。
/// @return 图廊根节点。
auto sample_gallery() -> Node {
    ensure_icons();

    Icon explicit_a{"square"};
    explicit_a.set_size(IconSize::S32).set_color(Color{37, 99, 235, 255});
    Icon theme_a{"plus"};  // 跟随主题 on_surface_variant：明暗两档取色不同
    theme_a.set_size(IconSize::S32);
    Icon explicit_b{"minus"};
    explicit_b.set_size(IconSize::S24).set_color(Color{220, 38, 38, 255});
    Icon theme_b{"frame"};  // 跟随主题 on_surface_variant
    theme_b.set_size(IconSize::S24);

    std::vector<Node> row_a;
    row_a.emplace_back(std::move(explicit_a));
    row_a.emplace_back(std::move(theme_a));
    row_a.emplace_back(std::move(explicit_b));
    row_a.emplace_back(std::move(theme_b));

    IconButton live{"plus"};
    live.set_size(IconSize::S32).set_on_click([] {});
    IconButton dead{"minus"};
    dead.set_size(IconSize::S32).set_on_click([] {}).set_enabled(false);

    std::vector<Node> row_b;
    row_b.emplace_back(std::move(live));
    row_b.emplace_back(std::move(dead));

    std::vector<Node> rows;
    rows.emplace_back(Row{RowProps{.children = std::move(row_a), .gap = 16.0F}});
    rows.emplace_back(Row{RowProps{.children = std::move(row_b), .gap = 16.0F}});
    return Node{std::make_shared<Column>(ColumnProps{.children = std::move(rows), .gap = 16.0F})};
}

/// 把图廊包进主题作用域。
/// @param theme 注入的主题。
/// @return 带主题作用域的根节点。
auto themed_gallery(const Theme &theme) -> Node { return Node{std::make_shared<ThemeScope>(theme, sample_gallery())}; }

/// 录制指定主题下图廊的 DisplayList 命令条数（完整 mount → layout → record paint）。
/// @param theme 生效主题。
/// @return 录制的命令条数。
auto record_cmd_count(const Theme &theme) -> std::size_t {
    constexpr BuildContext ctx;
    Node root = themed_gallery(theme);
    root->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(AURORA_CANVAS_W), .height = static_cast<float>(AURORA_CANVAS_H)};
    root->layout(c, ctx);

    Painter p;
    p.begin(AURORA_CANVAS_W, AURORA_CANVAS_H);
    DisplayList dl;
    p.record(dl);
    root->paint(
        p,
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
             .size = Size{.width = static_cast<float>(AURORA_CANVAS_W), .height = static_cast<float>(AURORA_CANVAS_H)}},
        ctx);
    p.stop();
    return dl.cmd_count();
}

/// 在指定主题下无头渲染图廊并取回像素。
/// @param theme 生效主题。
/// @return 渲染结果（RGBA8 图像）。
auto render_under(const Theme &theme) -> Image {
    Node root = themed_gallery(theme);
    return render_to_image(root, AURORA_CANVAS_W, AURORA_CANVAS_H);
}

}  // namespace

AURORA_TEST_CASE(theme_switch_reuses_display_list_structure) {
    // 同一棵树换肤只改颜色、不改几何：命令条数在明/暗两档下必须一致（结构复用），且为正。
    const std::size_t light = record_cmd_count(Theme::light());
    const std::size_t dark = record_cmd_count(Theme::dark());
    AURORA_TEST_CHECK_MSG(light > 0, "DL cmd_count > 0 under light theme");
    AURORA_TEST_CHECK_MSG(light == dark, "same icon tree reuses identical DL command count across light/dark");
    AURORA_TEST_PRINTF("  DL cmd_count: light=%zu dark=%zu\n", light, dark);
}

AURORA_TEST_CASE(theme_switch_changes_rendered_pixels) {
    // 跟随主题默认色的图标在换肤后必须重绘出不同像素（换肤重绘真实生效，而非停留在旧帧）。
    const Image light = render_under(Theme::light());
    const Image dark = render_under(Theme::dark());
    AURORA_TEST_CHECK_EQ(light.pixels.size(), dark.pixels.size());
    AURORA_TEST_CHECK_MSG(light.pixels != dark.pixels,
                          "theme-default icon colors change with theme => rendered pixels differ");
}

AURORA_TEST_CASE(golden_icon_gallery_light) {
    Node root = themed_gallery(Theme::light());
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_icon_gallery_light.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_CANVAS_W, AURORA_CANVAS_H, current_path.string().c_str()).ok());
    golden::compare_or_update("icon_gallery_light", current_path, &root);
}

AURORA_TEST_CASE(golden_icon_gallery_dark) {
    Node root = themed_gallery(Theme::dark());
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_icon_gallery_dark.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_CANVAS_W, AURORA_CANVAS_H, current_path.string().c_str()).ok());
    golden::compare_or_update("icon_gallery_dark", current_path, &root);
}

}  // namespace aurora::test_cases::itest_icon_theme
