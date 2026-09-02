// TextInput 控件 demo：value / placeholder。
#include "demo_common.h"

auto main() -> int {
    au::Node root = au::Column{
        GradientTitle{ "TextInput 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "单行文本输入" } },
        au::TextInput{ au::TextInputProps{ .value = "", .placeholder = "Type your name…" } },
        gap(8),
        au::Text{ au::LocalizedString{ "预填值示例" } },
        au::TextInput{ au::TextInputProps{ .value = "Ada Lovelace", .placeholder = "placeholder" } },
    };
    return run_demo(Card{ std::move(root) }, "TextInput · Aurora Demo", 520.0f, 380.0f);
}
