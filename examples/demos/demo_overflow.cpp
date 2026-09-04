// OverflowStrategy demo：展示不同溢出策略的效果对比。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::enable_dpi_awareness();
    au::init_console();

    // 辅助：创建一个固定尺寸容器，内含超出容器的子内容
    auto make_overflow_box = [](au::OverflowStrategy strategy, const char *label) -> au::Node {
        // 子内容：一个比容器大的红色背景 Text
        const auto txt = std::make_shared<au::Text>();
        txt->content = au::LocalizedString{std::string{label}};
        txt->modifier.set(au::Modifier{}.size(260.0F, 120.0F).background(au::Color{220, 50, 50, 255}));

        // 容器：固定 150x60，绿色背景
        au::Column col{au::ColumnProps{.children = {au::Node{txt}}}};
        col.modifier.set(au::Modifier{}
                             .size(150.0F, 60.0F)
                             .background(au::Color{50, 180, 80, 255})
                             .border(1.0F, pal::AURORA_BORDER));
        col.overflow_strategy(strategy);
        return col;
    };

    au::Node root = au::Column{
        GradientTitle{"OverflowStrategy"},
        gap(12),

        Card{
            au::Text{au::LocalizedString{"Visible (default): overflow child content visible"}},
            gap(8),
            make_overflow_box(au::OverflowStrategy::Visible, "Visible: content overflows"),
        },
        gap(12),

        Card{
            au::Text{au::LocalizedString{"Hidden: overflow part clipped"}},
            gap(8),
            make_overflow_box(au::OverflowStrategy::Hidden, "Hidden: content clipped"),
        },
        gap(12),

        Card{
            au::Text{au::LocalizedString{"Clip: same as Hidden, keeps hit-test (reserved semantics)"}},
            gap(8),
            make_overflow_box(au::OverflowStrategy::Clip, "Clip: clipped + hit-test"),
        },
        gap(12),

        Card{
            au::Text{au::LocalizedString{"Scroll: overflow scrollable (reserved, currently same as Hidden)"}},
            gap(8),
            make_overflow_box(au::OverflowStrategy::Scroll, "Scroll: scrollable (placeholder)"),
        },
    };

    return run_demo(Card{std::move(root)}, "OverflowStrategy · Aurora Demo", 520.0F, 680.0F);
}