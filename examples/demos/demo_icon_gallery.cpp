// Icon / IconButton 控件 demo（图标组件）：应用侧注册语义名 → SVG 资产，展示尺寸档位、
// 显式着色与明暗两套主题下的取色；库不内置任何图标资产，全部资产由本示例自行提供。
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "demo_common.h"

namespace {

/// 目标 SVG 画布边长（与 IconData 的光栅目标尺寸解耦：由控件按 set_size 重光栅化）。
constexpr int AURORA_SVG_BOX = 32;

/// 以只含 `<rect>` 的最小 SVG 描述一个图标（内置 SVG 子集光栅器支持）。
/// @param size 画布边长。
/// @param rects 若干 [x, y, w, h] 矩形（黑色填充，最终由控件着色覆盖）。
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

/// 注册示例图标集（应用侧语义名 → IconData），库侧不预置任何条目。
auto register_sample_icons() -> void {
    au::icon::IconRegistry::clear();
    au::icon::IconRegistry::register_icon(
        "square", au::icon::IconData::svg(svg_from_rects(AURORA_SVG_BOX, {{4, 4, 24, 24}}), "square"));
    au::icon::IconRegistry::register_icon(
        "plus", au::icon::IconData::svg(svg_from_rects(AURORA_SVG_BOX, {{12, 4, 8, 24}, {4, 12, 24, 8}}), "plus"));
    au::icon::IconRegistry::register_icon(
        "minus", au::icon::IconData::svg(svg_from_rects(AURORA_SVG_BOX, {{6, 14, 20, 4}}), "minus"));
    au::icon::IconRegistry::register_icon(
        "frame",
        au::icon::IconData::svg(
            svg_from_rects(AURORA_SVG_BOX, {{4, 4, 24, 3}, {4, 25, 24, 3}, {4, 4, 3, 24}, {25, 4, 3, 24}}), "frame"));
}

/// 一行尺寸档位演示：同一 SVG 资产在 16 / 20 / 24 / 32 dp 下的绘制。
auto sizes_row(const au::Color &accent) -> au::Node {
    au::Icon s16{"square"};
    s16.set_size(au::IconSize::S16);
    au::Icon s20{"plus"};
    s20.set_size(au::IconSize::S20).set_color(accent);
    au::Icon s24{"minus"};
    s24.set_size(au::IconSize::S24);
    au::Icon s32{"frame"};
    s32.set_size(au::IconSize::S32).set_color(accent);
    return au::Row{
        au::RowProps{.children = {std::move(s16), std::move(s20), std::move(s24), std::move(s32)}, .gap = 24.0F}};
}

/// 一行 IconButton 演示：常态 + 禁用态，覆盖四态取色的静态可见部分。
auto buttons_row() -> au::Node {
    au::IconButton normal{"plus"};
    normal.set_on_click([] {});
    normal.set_semantic_label("Add");
    au::IconButton disabled{"minus"};
    disabled.set_on_click([] {}).set_enabled(false).set_semantic_label("Remove");
    au::IconButton framed{"frame"};
    framed.set_on_click([] {}).set_semantic_label("Frame");
    return au::Row{au::RowProps{.children = {std::move(normal), std::move(disabled), std::move(framed)}, .gap = 16.0F}};
}

/// 单侧（明或暗）主题下的整段图廊正文。
auto gallery_body(const au::Color &accent) -> au::Node {
    return au::Column{au::ColumnProps{
        .children =
            {
                au::Text{"Sizes: 16 / 20 / 24 / 32 dp"},
                sizes_row(accent),
                au::Text{"IconButton: normal / disabled / frame"},
                buttons_row(),
            },
        .gap = 16.0F,
    }};
}

/// 把一段正文包进指定主题作用域。
auto themed(const au::Theme &theme, au::Node child) -> au::Node {
    return au::Node{std::make_shared<au::ThemeScope>(theme, std::move(child))};
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做 try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    register_sample_icons();

    const au::Color light_accent{37, 99, 235};
    const au::Color dark_accent{147, 197, 253};

    au::Node light_side = themed(au::Theme::light(), gallery_body(light_accent));
    au::Node dark_side = themed(au::Theme::dark(), gallery_body(dark_accent));

    au::Node root = au::Column{au::ColumnProps{
        .children =
            {
                GradientTitle{"Icon gallery"},
                gap(8),
                au::Text{"Light theme"},
                std::move(light_side),
                gap(12),
                au::Text{"Dark theme"},
                std::move(dark_side),
            },
        .gap = 12.0F,
    }};

    return run_demo(Card{std::move(root)}, "Icon gallery - Aurora Demo", 640.0F, 560.0F);
}
