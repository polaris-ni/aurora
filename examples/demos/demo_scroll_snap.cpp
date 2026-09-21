// 滚动吸附 demo：Scroll 整页翻页（ScrollSnap::page）+ LazyList 条目吸附（set_snap）。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    // ① 分页：每页高 = 视口高，滚轮落点后短滑动收敛到整页边界（reduce-motion 下直落）。
    constexpr float aurora_page_height = 180.0F;
    std::vector<au::Node> pages;
    for (int i = 0; i < 5; ++i) {
        au::Text label{"Page " + std::to_string(i + 1) + " · snap paging"};
        label.modifier.set(au::Modifier{}.padding(12.0F));
        au::Column page{au::ColumnProps{.children = {std::move(label)}}};
        page.modifier.set(au::Modifier{}
                              .size(360.0F, aurora_page_height)
                              .background(i % 2 == 0 ? pal::AURORA_SURFACE : pal::AURORA_BORDER)
                              .border(1.0F, pal::AURORA_BORDER));
        pages.emplace_back(std::move(page));
    }

    au::Scroll pager{au::ScrollProps{
        .child = au::Node{au::Column{au::ColumnProps{.children = std::move(pages)}}},
        .snap = au::ScrollSnap::page(),
    }};
    pager.modifier.set(au::Modifier{}.size(360.0F, aurora_page_height).border(1.0F, pal::AURORA_BORDER));

    // ② 条目吸附：周期 = 行高，居中对齐（末段不足一格时夹到内容末端，不停在半行）。
    auto list = std::make_shared<au::LazyList>(
        200,
        [](int i) -> au::Node {
            au::Text item{"Row " + std::to_string(i) + " · snap to center"};
            item.modifier.set(au::Modifier{}.padding(8.0F));
            return au::Node{std::move(item)};
        },
        32.0F);
    list->set_snap(au::ScrollSnap{.extent = 32.0F, .alignment = au::ScrollSnapAlignment::Center});
    list->modifier.set(au::Modifier{}.size(360.0F, 128.0F).border(1.0F, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{"Scroll snapping & paging"},
        gap(8),
        au::Text{"Wheel up/down: glides to the nearest page / row boundary"},
        std::move(pager),
        gap(12),
        std::shared_ptr<au::Widget>{list},
    };
    return run_demo(Card{std::move(root)}, "ScrollSnap · Aurora Demo", 520.0F, 520.0F);
}
