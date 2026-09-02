// RichTextEdit 控件测试：文档模型、编辑操作、UndoStack 集成、序列化。

#include "aurora/state/undo_stack.h"
#include "aurora/widget/rich_text_edit.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Json;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::RichTextEdit;
using aurora::TextInputEvent;
using aurora::TextSpan;
using aurora::UndoStack;

AURORA_TEST() {

    // ---- 基本构造 + 空文档 ----
    {
        RichTextEdit ed;
        AURORA_TEST_CHECK(ed.plain_text().empty());
        AURORA_TEST_CHECK(ed.caret() == 0);
        AURORA_TEST_CHECK(!ed.has_selection());
        AURORA_TEST_CHECK(ed.type_name() == std::string("RichTextEdit"));
    }

    // ---- load_spans + plain_text ----
    {
        RichTextEdit ed;
        std::vector<TextSpan> spans;
        TextSpan s1;
        s1.text.text = "Hello";
        s1.font.size_pt = 18.0f;
        s1.color = Color::blue();
        spans.push_back(s1);
        TextSpan s2;
        s2.text.text = " World";
        s2.font.weight = 700;
        spans.push_back(s2);
        ed.load_spans(spans);
        AURORA_TEST_CHECK(ed.plain_text() == "Hello World");
        AURORA_TEST_CHECK(ed.caret() == 0);
    }

    // ---- to_spans 合并连续同样式字符 ----
    {
        RichTextEdit ed;
        std::vector<TextSpan> spans;
        TextSpan s;
        s.text.text = "ABC";
        s.font.size_pt = 14.0f;
        spans.push_back(s);
        ed.load_spans(spans);
        const auto out = ed.to_spans();
        AURORA_TEST_CHECK(out.size() == 1);
        AURORA_TEST_CHECK(out[0].text.text == "ABC");
    }

    // ---- toggle_bold / toggle_underline ----
    {
        RichTextEdit ed;
        AURORA_TEST_CHECK(ed.current_font().weight == 400);
        ed.toggle_bold();
        AURORA_TEST_CHECK(ed.current_font().weight == 700);
        ed.toggle_bold();
        AURORA_TEST_CHECK(ed.current_font().weight == 400);

        AURORA_TEST_CHECK(!ed.current_underline());
        ed.toggle_underline();
        AURORA_TEST_CHECK(ed.current_underline());
    }

    // ---- UndoStack 基本集成 ----
    {
        RichTextEdit ed;
        UndoStack stack;
        ed.set_undo_stack(&stack);

        // 从空文档开始，通过 load_spans 加载初始内容（不走 undo）
        std::vector<TextSpan> spans;
        TextSpan s;
        s.text.text = "AB";
        spans.push_back(s);
        ed.load_spans(spans);

        // 模拟焦点（on_text_input 检查 is_focused）
        ed.on_focus_change(true);
        // 将光标移到末尾，以便插入 "C" 后得到 "ABC"
        // 通过模拟 End 键
        KeyEvent ke;
        ke.action = KeyAction::Down;
        ke.key = static_cast<int>(KeyCode::End);
        ke.modifiers = {};
        ed.on_key_event(ke);
        AURORA_TEST_CHECK(ed.caret() == 2); // 确认光标在末尾

        // 模拟键盘输入 "C" — 通过 on_text_input 模拟
        TextInputEvent tie;
        tie.text = "C";
        ed.on_text_input(tie);
        AURORA_TEST_CHECK(ed.plain_text() == "ABC");
        AURORA_TEST_CHECK(stack.can_undo());
        AURORA_TEST_CHECK(stack.count() == 1);

        // 撤销
        stack.undo();
        AURORA_TEST_CHECK(ed.plain_text() == "AB");

        // 重做
        stack.redo();
        AURORA_TEST_CHECK(ed.plain_text() == "ABC");
    }

    // ---- describe_static ----
    {
        const auto desc = RichTextEdit::describe_static();
        AURORA_TEST_CHECK(desc.name == "RichTextEdit");
    }

    // ---- 序列化往返 ----
    {
        RichTextEdit ed;
        std::vector<TextSpan> spans;
        TextSpan s;
        s.text.text = "Test";
        spans.push_back(s);
        ed.load_spans(spans);

        Json props;
        ed.serialize_props(props);
        AURORA_TEST_CHECK(props.contains("text"));
        AURORA_TEST_CHECK(props["text"].get<std::string>() == "Test");

        RichTextEdit ed2;
        ed2.deserialize_props(props);
        AURORA_TEST_CHECK(ed2.plain_text() == "Test");
    }
}
