// Modifier 控件 demo：Padding / Background / Border / Clip / Size / Clickable / Align / fillMax。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Text padded{au::LocalizedString{"padding + background + border"}};
    padded.modifier.set(
        au::Modifier{}.padding(14.0F).background(pal::AURORA_PRIMARY_SOFT).border(1.0F, pal::AURORA_BORDER));

    au::Text clipped{au::LocalizedString{"Long text clipped to 160x28"}};
    clipped.modifier.set(
        au::Modifier{}.size(160.0F, 28.0F).background(pal::AURORA_SURFACE).clip().border(1.0F, pal::AURORA_BORDER));

    au::Text clickable{au::LocalizedString{"clickable (tap callback)"}};
    clickable.modifier.set(au::Modifier{}.padding(10.0F).background(pal::AURORA_OK).clickable([]() -> void {
        AURORA_LOG_INFO("demo", "[modifiers] clicked");
    }));

    au::Text sized{au::LocalizedString{"size(120,40) + align"}};
    sized.modifier.set(au::Modifier{}.size(120.0F, 40.0F).background(pal::AURORA_ACCENT).align(au::Alignment::Center));

    au::Text fill{au::LocalizedString{"fill_max_width"}};
    fill.modifier.set(au::Modifier{}.fill_max_width().padding(8.0F).background(pal::AURORA_WARN));

    au::Node root = au::Column{
        GradientTitle{"Modifier widget"},
        gap(10),
        std::move(padded),
        gap(8),
        std::move(clipped),
        gap(8),
        std::move(clickable),
        gap(8),
        std::move(sized),
        gap(8),
        std::move(fill),
    };
    return run_demo(Card{std::move(root)}, "Modifier · Aurora Demo", 520.0F, 520.0F);
}