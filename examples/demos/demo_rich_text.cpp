// RichText 控件 demo：多片段带样式文本 + 自动换行（确定性贪心换行）。
#include <memory>
#include <vector>

#include "aurora/widget/rich_text.h"
#include "aurora/widget/text_span.h"

#include "demo_common.h"

auto main() -> int {
    using au::TextSpan;
    auto spans = std::vector{
        TextSpan{ .text = au::LocalizedString{ "富文本标题 " },
                  .font = au::Font{ .size_pt = 20.0f, .weight = 700 },
                  .color = pal::AURORA_PRIMARY },
        TextSpan{ .text = au::LocalizedString{ "普通正文，支持 " }, .font = au::Font{}, .color = pal::AURORA_TEXT },
        TextSpan{ .text = au::LocalizedString{ "红色强调" },
                  .font = au::Font{ .size_pt = 16.0f },
                  .color = au::Color{ 200, 0, 0, 255 } },
        TextSpan{ .text = au::LocalizedString{ " 与静音色收尾。" },
                  .font = au::Font{ .size_pt = 14.0f },
                  .color = pal::AURORA_MUTED },
    };
    const auto state = std::make_shared<au::State<std::vector<TextSpan>>>(std::move(spans));
    au::RichText rt{ au::Reactive{ state } };

    au::Node root = au::Column{
        GradientTitle{ "RichText 控件" },
        gap(12),
        std::move(rt),
    };
    return run_demo(Card{ std::move(root) }, "RichText · Aurora Demo", 520.0f, 360.0f);
}
