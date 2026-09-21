// StickyHeader 吸顶头部 demo：分组标题滚到视口顶部时钉驻，后到标题把前一个向上顶出。
//
// 头部本身正常参与布局（占据自身高度、随内容滚动），宿主 `Scroll` 在合成内容后把「已滚过头顶」
// 的头部按 pin 位重绘于顶部覆盖层——内容缓冲不因此逐帧重录。纯绘制层实现，故与 reduce-motion、
// snap 收位等滚动通道互不干扰。
#include <memory>
#include <string>
#include <vector>

#include "aurora/widget/sticky_header.h"
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto header = [](const std::string &title) -> au::Node {
        au::Text label{au::LocalizedString{title}};
        label.bold().color(pal::AURORA_TEXT);
        label.modifier.set(au::Modifier{}.padding(8.0F).background(pal::AURORA_PRIMARY_SOFT));
        return au::StickyHeader{au::Node{std::move(label)}};
    };
    auto row = [](const std::string &text) -> au::Node {
        au::Text item{au::LocalizedString{text}};
        item.modifier.set(
            au::Modifier{}.padding(6.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));
        return item;
    };

    std::vector<au::Node> kids;
    for (int group = 0; group < 4; ++group) {
        kids.emplace_back(header("Group " + std::to_string(group)));
        for (int i = 0; i < 6; ++i) {
            kids.emplace_back(row("  entry " + std::to_string(i)));
        }
    }

    au::Scroll scroll{au::ScrollProps{.child = au::Node{au::Column{au::ColumnProps{.children = std::move(kids)}}}}};
    scroll.modifier.set(au::Modifier{}.size(340.0F, 260.0F).border(1.0F, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{"StickyHeader"},
        gap(12),
        au::Text{au::LocalizedString{"滚动列表：分组标题钉驻顶部，下一个标题把上一个顶出"}},
        gap(8),
        Card{std::move(scroll)},
    };
    return run_demo(Card{std::move(root)}, "StickyHeader · Aurora Demo", 520.0F, 460.0F);
}
