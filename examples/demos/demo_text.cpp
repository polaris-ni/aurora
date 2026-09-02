// Text 控件 demo：字号、字重、颜色、LocalizedString、字距/词距、斜体、两端对齐(Justify)。
#include "demo_common.h"

auto main() -> int {
    const au::LocalizedString k_para = "The quick brown fox jumps over the lazy dog while a silent river flows "
                                       "beyond the quiet hills and the pale moon rises above the sleeping town.";
    au::Node root = au::Column{
        GradientTitle{ "Text 控件" },
        gap(12),
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "默认 14pt 文本" }, .text_color = pal::AURORA_TEXT } },
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "粗体 22pt 蓝色" },
                                 .font = au::Font{ .size_pt = 22.0f, .weight = 700 },
                                 .text_color = pal::AURORA_PRIMARY } },
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "静音色 16pt" },
                                 .font = au::Font{ .size_pt = 16.0f },
                                 .text_color = pal::AURORA_MUTED } },
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "强调色 18pt" },
                                 .font = au::Font{ .size_pt = 18.0f },
                                 .text_color = pal::AURORA_ACCENT } },
        gap(8),
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "字距 4dp · 词距 8dp（与光标/选区/命中一致）" },
                                 .font = au::Font{ .size_pt = 16.0f },
                                 .text_color = pal::AURORA_TEXT,
                                 .letter_spacing = 4.0f,
                                 .word_spacing = 8.0f } },
        au::Text{ au::TextProps{ .content = au::LocalizedString{ "斜体 Italic（FontStyle::Italic）" },
                                 .font = au::Font{ .size_pt = 16.0f },
                                 .text_color = pal::AURORA_ACCENT,
                                 .font_style = au::FontStyle::Italic } },
        gap(8),
        au::Text{ au::TextProps{ .content = k_para,
                                 .font = au::Font{ .size_pt = 15.0f },
                                 .text_color = pal::AURORA_TEXT,
                                 .text_align = au::TextAlign::Justify,
                                 .soft_wrap = true } },
    };
    return run_demo(Card{ std::move(root) }, "Text · Aurora Demo", 520.0f, 460.0f);
}
