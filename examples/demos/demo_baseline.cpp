// CrossAxisAlignment::Baseline demo：Row 内子项按首行文本基线对齐。
// 覆盖三种典型场景：不同字号混排、无基线子项（图标，走 CSS 式合成基线 = 自身底边）、
// 带 Modifier 内边距的文本与 Button 混排。
#include "demo_common.h"

namespace {

/// 无基线子项（示意图标）：自身不提供 baseline_distance，容器按合成基线（其底边）参与对齐。
auto icon_box(float size) -> au::Node {
    au::Column box;
    box.modifier.set(au::Modifier{}.size(size, size).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
    return au::Node{std::move(box)};
}

/// 带边框的对照行：便于目视基线是否共线（对齐方式由子行自行声明）。
auto framed(const au::Node &child) -> au::Node {
    au::Row row;
    row.add(child);
    row.modifier.set(au::Modifier{}.background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
    return au::Node{std::move(row)};
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // ① 不同字号：小字号的布局盒整体下移，使首行基线与大字号共线。
    au::Row mixed;
    auto small = au::Text{au::LocalizedString{"12pt"}};
    small.font_size(12.0F).color(pal::AURORA_TEXT);
    auto medium = au::Text{au::LocalizedString{"18pt"}};
    medium.font_size(18.0F).color(pal::AURORA_TEXT);
    auto large = au::Text{au::LocalizedString{"28pt"}};
    large.font_size(28.0F).color(pal::AURORA_TEXT);
    mixed.add(au::Node{std::move(small)});
    mixed.add(au::Node{std::move(medium)});
    mixed.add(au::Node{std::move(large)});
    mixed.set_cross_axis_alignment(au::CrossAxisAlignment::Baseline);
    mixed.set_gap(12.0F);

    // ② 无基线子项：图标（Column，无 baseline_distance 覆写）以底边为合成基线对齐文字基线。
    au::Row with_icon;
    auto label = au::Text{au::LocalizedString{"Icon + label"}};
    label.font_size(20.0F).color(pal::AURORA_TEXT);
    with_icon.add(icon_box(20.0F));
    with_icon.add(au::Node{std::move(label)});
    with_icon.set_cross_axis_alignment(au::CrossAxisAlignment::Baseline);
    with_icon.set_gap(8.0F);

    // ③ 内边距与按钮：Modifier 内边距把内容盒下移，容器补入该位移后基线仍共线；
    //    Button 的钩子与 paint_label 同源（标签居中偏移 + ascent）。
    au::Row with_button;
    auto padded = au::Text{au::LocalizedString{"padded text"}};
    padded.font_size(16.0F).color(pal::AURORA_TEXT);
    padded.modifier.set(au::Modifier{}.padding(10.0F));
    with_button.add(au::Node{std::move(padded)});
    with_button.add(au::Node{au::Button{"Button"}});
    with_button.set_cross_axis_alignment(au::CrossAxisAlignment::Baseline);
    with_button.set_gap(12.0F);

    au::Node root = au::Column{
        GradientTitle{"CrossAxisAlignment::Baseline"},
        au::Text{au::LocalizedString{"Mixed font sizes (12 / 18 / 28pt)"}},
        framed(std::move(mixed)),
        gap(12),
        au::Text{au::LocalizedString{"Item without baseline (icon) synthesizes its bottom edge"}},
        framed(std::move(with_icon)),
        gap(12),
        au::Text{au::LocalizedString{"Modifier padding + Button label"}},
        framed(std::move(with_button)),
        gap(12),
        au::Text{au::LocalizedString{"Note: Column + Baseline has no baseline semantics (cross axis is horizontal); "
                                     "it falls back to Start and reports one degraded diagnostic."}},
    };
    return run_demo(Card{std::move(root)}, "CrossAxisAlignment::Baseline · Aurora Demo", 560.0F, 420.0F);
}
