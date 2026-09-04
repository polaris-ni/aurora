// Spacer 控件 demo：吸收剩余空间，把相邻控件推向两端。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Column top{
        au::Text{au::LocalizedString{"Top"}},
        au::Spacer{},
        au::Text{au::LocalizedString{"Bottom (Spacer absorbs middle space)"}},
    };
    top.modifier.set(
        au::Modifier{}.size(320.0F, 200.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));

    au::Row row{
        au::Text{au::LocalizedString{"Left"}},
        au::Spacer{},
        au::Text{au::LocalizedString{"Right"}},
    };
    row.modifier.set(
        au::Modifier{}.size(320.0F, 48.0F).background(pal::AURORA_SURFACE).border(1.0F, pal::AURORA_BORDER));

    au::Node root = au::Column{
        GradientTitle{"Spacer widget"}, gap(12), std::move(top), gap(12), std::move(row),
    };
    return run_demo(Card{std::move(root)}, "Spacer · Aurora Demo", 520.0F, 480.0F);
}