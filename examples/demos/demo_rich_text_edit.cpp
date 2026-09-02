// RichTextEdit 控件 demo：可编辑富文本，支持粗体/斜体/下划线/撤销重做。
#include "demo_common.h"

auto main() -> int {
    au::UndoStack undo_stack;

    auto editor = std::make_shared<au::RichTextEdit>();
    editor->set_undo_stack(&undo_stack);

    // 初始内容
    std::vector<au::TextSpan> initial;
    au::TextSpan title;
    title.text.text = "Aurora RichTextEdit";
    title.font.size_pt = 20.0f;
    title.font.weight = 700;
    title.color = au::Color::blue();
    initial.push_back(title);

    au::TextSpan body;
    body.text.text = "\n支持粗体(Ctrl+B)、斜体(Ctrl+I)、下划线(Ctrl+U)、撤销(Ctrl+Z)、重做(Ctrl+Y)。";
    body.font.size_pt = 14.0f;
    initial.push_back(body);

    editor->load_spans(initial);

    au::Node root = au::Column{
        GradientTitle{ "RichTextEdit" },
        gap(12),
        au::Text{ "快捷键: Ctrl+B 粗体 | Ctrl+I 斜体 | Ctrl+U 下划线 | Ctrl+Z 撤销 | Ctrl+Y 重做" },
        au::Node{ std::static_pointer_cast<au::Widget>(editor) },
    };
    return run_demo(Card{ std::move(root) }, "RichTextEdit · Aurora Demo", 640.0f, 400.0f);
}
