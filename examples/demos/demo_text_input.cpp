// TextInput 控件 demo：value / placeholder。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Node root = au::Column{
        GradientTitle{"TextInput widget"},
        gap(12),
        au::Text{au::LocalizedString{"Single-line text input"}},
        au::TextInput{au::TextInputProps{.value = "", .placeholder = "Type your name…"}},
        gap(8),
        au::Text{au::LocalizedString{"Pre-filled value example"}},
        au::TextInput{au::TextInputProps{.value = "Ada Lovelace", .placeholder = "placeholder"}},
    };
    return run_demo(Card{std::move(root)}, "TextInput · Aurora Demo", 520.0F, 380.0F);
}