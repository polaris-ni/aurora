// Modifier 控件 demo：Padding / Background / Border / Clip / Size / Clickable / Align / fillMax。
#include "demo_common.h"

auto main() -> int {
    au::Text padded{ au::LocalizedString{ "padding + background + border" } };
    padded.modifier.set(
        au::Modifier{}.padding(14.0f).background(pal::AURORA_PRIMARY_SOFT).border(1.0f, pal::AURORA_BORDER));

    au::Text clipped{ au::LocalizedString{ "长文本被 clip 裁剪到 160x28" } };
    clipped.modifier.set(
        au::Modifier{}.size(160.0f, 28.0f).background(pal::AURORA_SURFACE).clip().border(1.0f, pal::AURORA_BORDER));

    au::Text clickable{ au::LocalizedString{ "clickable（点击有回调）" } };
    clickable.modifier.set(au::Modifier{}.padding(10.0f).background(pal::AURORA_OK).clickable([]() -> void {
        AURORA_LOG_INFO("demo", "[modifiers] clicked");
    }));

    au::Text sized{ au::LocalizedString{ "size(120,40) + align" } };
    sized.modifier.set(au::Modifier{}.size(120.0f, 40.0f).background(pal::AURORA_ACCENT).align(au::Alignment::Center));

    au::Text fill{ au::LocalizedString{ "fill_max_width" } };
    fill.modifier.set(au::Modifier{}.fill_max_width().padding(8.0f).background(pal::AURORA_WARN));

    au::Node root = au::Column{
        GradientTitle{ "Modifier 控件" },
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
    return run_demo(Card{ std::move(root) }, "Modifier · Aurora Demo", 520.0f, 520.0f);
}
