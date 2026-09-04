// RichTextEdit 控件 demo：可编辑富文本，支持粗体/斜体/下划线/撤销重做。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::UndoStack undo_stack;

    auto editor = std::make_shared<au::RichTextEdit>();
    editor->set_undo_stack(&undo_stack);

    // 初始内容
    std::vector<au::TextSpan> initial;
    au::TextSpan title;
    title.text.text = "Aurora RichTextEdit";
    title.font.size_pt = 20.0F;
    title.font.weight = 700;
    title.color = au::Color::blue();
    initial.push_back(title);

    au::TextSpan body;
    body.text.text = "\nSupports bold (Ctrl+B), italic (Ctrl+I), underline (Ctrl+U), undo (Ctrl+Z), redo (Ctrl+Y).";
    body.font.size_pt = 14.0F;
    initial.push_back(body);

    editor->load_spans(initial);

    au::Node root = au::Column{
        GradientTitle{"RichTextEdit"},
        gap(12),
        au::Text{"Shortcuts: Ctrl+B bold | Ctrl+I italic | Ctrl+U underline | Ctrl+Z undo | Ctrl+Y redo"},
        au::Node{std::static_pointer_cast<au::Widget>(editor)},
    };
    return run_demo(Card{std::move(root)}, "RichTextEdit · Aurora Demo", 640.0F, 400.0F);
}