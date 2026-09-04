// Divider 控件 demo：分隔线（横向/纵向）+ 缩进。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Node root = au::Column{
        GradientTitle{"Divider widget"},
        gap(12),
        au::Text{au::LocalizedString{"List item A"}},
        au::Divider{au::DividerProps{.indent = 16.0F, .end_indent = 16.0F}},
        au::Text{au::LocalizedString{"List item B (with left/right indent)"}},
        gap(12),
        au::Text{au::LocalizedString{"Two columns with a vertical divider in the middle:"}},
        au::Row{
            au::Text{au::LocalizedString{"Left column content"}},
            au::Divider{au::DividerProps{.orientation = au::Orientation::Vertical, .thickness = 2.0F}},
            au::Text{au::LocalizedString{"Right column content"}},
        },
    };
    return run_demo(Card{std::move(root)}, "Divider · Aurora Demo", 480.0F, 380.0F);
}