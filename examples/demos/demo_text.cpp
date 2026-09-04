// Text 控件 demo：字号、字重、颜色、LocalizedString、字距/词距、斜体、两端对齐(Justify)。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const au::LocalizedString k_para =
        "The quick brown fox jumps over the lazy dog while a silent river flows "
        "beyond the quiet hills and the pale moon rises above the sleeping town.";
    au::Node root = au::Column{
        GradientTitle{"Text widget"},
        gap(12),
        au::Text{au::TextProps{.content = au::LocalizedString{"Default 14pt text"}, .text_color = pal::AURORA_TEXT}},
        au::Text{au::TextProps{.content = au::LocalizedString{"Bold 22pt blue"},
                               .font = au::Font{.size_pt = 22.0F, .weight = 700},
                               .text_color = pal::AURORA_PRIMARY}},
        au::Text{au::TextProps{.content = au::LocalizedString{"Muted 16pt"},
                               .font = au::Font{.size_pt = 16.0F},
                               .text_color = pal::AURORA_MUTED}},
        au::Text{au::TextProps{.content = au::LocalizedString{"Accent 18pt"},
                               .font = au::Font{.size_pt = 18.0F},
                               .text_color = pal::AURORA_ACCENT}},
        gap(8),
        au::Text{au::TextProps{
            .content =
                au::LocalizedString{"Letter spacing 4dp · word spacing 8dp (consistent with cursor/selection/hit)"},
            .font = au::Font{.size_pt = 16.0F},
            .text_color = pal::AURORA_TEXT,
            .letter_spacing = 4.0F,
            .word_spacing = 8.0F}},
        au::Text{au::TextProps{.content = au::LocalizedString{"Italic (FontStyle::Italic)"},
                               .font = au::Font{.size_pt = 16.0F},
                               .text_color = pal::AURORA_ACCENT,
                               .font_style = au::FontStyle::Italic}},
        gap(8),
        au::Text{au::TextProps{.content = k_para,
                               .font = au::Font{.size_pt = 15.0F},
                               .text_color = pal::AURORA_TEXT,
                               .text_align = au::TextAlign::Justify,
                               .soft_wrap = true}},
    };
    return run_demo(Card{std::move(root)}, "Text · Aurora Demo", 520.0F, 460.0F);
}