// text_input_test.cpp — 覆盖 TextInput 富属性：链式 setter、默认值、序列化往返。

#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/text_input.h"

#include "test_harness.h"

namespace render = aurora::render;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::EdgeInsets;
using aurora::Font;
using aurora::Json;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::TextInput;
using aurora::TextInputEvent;

static void test_chained_setters() {
    TextInput t;
    t.set_value("x")
        .set_corner_radius(6.0f)
        .set_padding(EdgeInsets{ .left = 10.0f, .top = 10.0f, .right = 10.0f, .bottom = 10.0f })
        .set_cursor_color(Color::red())
        .set_enabled(false);

    Json j;
    t.serialize_props(j);
    AURORA_TEST_CHECK_MSG(near_f(j["corner_radius"].get<float>(), 6.0f), "corner_radius set -> json 6");
    AURORA_TEST_CHECK_MSG(near_f(j["padding"]["left"].get<float>(), 10.0f), "padding set -> json 10");
    AURORA_TEST_CHECK_MSG(j["cursor_color"].is_array() && j["cursor_color"][0].get<int>() == 255 &&
                              j["cursor_color"][1].get<int>() == 0,
                          "cursor_color set -> json red");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == false, "enabled set -> json false");
}

static void test_serialize_roundtrip() {
    TextInput a;
    a.set_value("hello")
        .set_placeholder("ph")
        .font_size(18.0f)
        .set_corner_radius(4.0f)
        .set_padding(EdgeInsets{ .left = 2.0f, .top = 4.0f, .right = 6.0f, .bottom = 8.0f })
        .set_cursor_color(Color{ 10, 20, 30, 255 })
        .set_enabled(false);

    Json j;
    a.serialize_props(j);
    TextInput b;
    b.deserialize_props(j);

    Json k;
    b.serialize_props(k);

    AURORA_TEST_CHECK_MSG(k["value"].get<std::string>() == "hello", "rt value");
    AURORA_TEST_CHECK_MSG(k["placeholder"].get<std::string>() == "ph", "rt placeholder");
    AURORA_TEST_CHECK_MSG(near_f(k["font_size"].get<float>(), 18.0f), "rt font_size");
    AURORA_TEST_CHECK_MSG(near_f(k["corner_radius"].get<float>(), 4.0f), "rt corner_radius");
    AURORA_TEST_CHECK_MSG(near_f(k["padding"]["top"].get<float>(), 4.0f), "rt padding.top");
    AURORA_TEST_CHECK_MSG(near_f(k["padding"]["bottom"].get<float>(), 8.0f), "rt padding.bottom");
    AURORA_TEST_CHECK_MSG(k["cursor_color"][2].get<int>() == 30, "rt cursor_color.b=30");
    AURORA_TEST_CHECK_MSG(k["enabled"].get<bool>() == false, "rt enabled");
}

static void test_defaults() {
    const TextInput t;
    Json j;
    t.serialize_props(j);
    AURORA_TEST_CHECK_MSG(near_f(j["corner_radius"].get<float>(), 0.0f), "default corner_radius 0");
    AURORA_TEST_CHECK_MSG(near_f(j["padding"]["left"].get<float>(), 12.0f), "default padding 12");
    AURORA_TEST_CHECK_MSG(j["cursor_color"][0].get<int>() == 0, "default cursor_color black");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == true, "default enabled true");
}

static void test_selection_endpoint_highlight() {
    // 回归：选中整段后，行尾字符（最后一个字符）必须被高亮——此前旧半开区间
    // 模型 + hit_test_char（按中点）会让松手落在字符左半的端点字符漏选。
    render::FontEngine::instance().set_text_aa_mode(render::TextAAMode::Supersample);
    TextInput ti;
    ti.set_value("Hello World").font_size(24).set_padding(EdgeInsets{ .left = 0, .top = 0, .right = 0, .bottom = 0 });
    BuildContext ctx;
    ti.mount(ctx);
    Constraints cc;
    cc.min = Size{ .width = 0, .height = 0 };
    cc.max = Size{ .width = 400, .height = 60 };
    const Size sz = ti.layout(cc, ctx);
    const Font f{ .size_pt = 24.0f };
    constexpr render::TextLayoutOpts o{};
    const float full = render::FontEngine::instance().measure_width("Hello World", f, o);
    // 'd' 的起点 x 与宽度（用于把松手点落在 'd' 的左半——旧模型会因此漏掉 'd'）。
    const float up_to_d = render::FontEngine::instance().caret_x("Hello World", 10, f, o);
    const float w_d = full - up_to_d;
    const float release_x = up_to_d + (w_d * 0.25f); // 'd' 左四分之一处

    // 从文本最左拖到 'd' 的左半（端点字符的左半落点正是旧模型的漏选点）。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{ .x = 0.0f, .y = 5.0f };
    ti.on_pointer_event(press);
    MouseEvent move;
    move.action = MouseAction::Move;
    move.button = MouseButton::Left;
    move.local_position = Point{ .x = release_x, .y = 5.0f };
    ti.on_pointer_event(move);
    MouseEvent rel;
    rel.action = MouseAction::Release;
    rel.button = MouseButton::Left;
    rel.local_position = Point{ .x = release_x, .y = 5.0f };
    ti.on_pointer_event(rel);

    AURORA_TEST_CHECK(ti.has_selection());
    AURORA_TEST_CHECK(ti.selected_text() == "Hello World"); // 含尾：全 11 个字符

    Painter p;
    p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
    p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = sz.width, .height = sz.height } },
                Color::white());
    ti.paint(p, Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = sz.width, .height = sz.height } }, ctx);

    auto is_blue = [](const Color &c) -> bool { return static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30; };
    int minx = 1e9;
    int maxx = -1e9;
    for (int y = 0; y < static_cast<int>(sz.height); ++y) {
        for (int x = 0; x < static_cast<int>(sz.width); ++x) {
            if (is_blue(p.get_pixel(x, y))) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
            }
        }
    }
    AURORA_TEST_CHECK(minx <= 2);                          // 行首字符被高亮
    AURORA_TEST_CHECK(maxx >= static_cast<int>(full) - 2); // 行尾字符（'d'）被高亮
    AURORA_TEST_PRINTF("[SEL] minx=%d maxx=%d full=%.1f\n", minx, maxx, full);
}

static void test_behavior_props() {
    // max_length：超出部分截断；on_changed 每次编辑触发
    TextInput ti;
    int changed = 0;
    ti.set_max_length(5).set_on_changed([&](const std::string & /*v*/) -> void { ++changed; });
    ti.on_focus_change(true);
    TextInputEvent e1;
    e1.text = "Hello";
    ti.on_text_input(e1);
    AURORA_TEST_CHECK_MSG(ti.value() == "Hello" && changed == 1, "TextInput: input fires on_changed");
    TextInputEvent e2;
    e2.text = "World";
    ti.on_text_input(e2);
    AURORA_TEST_CHECK_MSG(ti.value() == "Hello", "TextInput: max_length=5 truncates subsequent input");
    AURORA_TEST_CHECK_MSG(changed == 1, "TextInput: truncated input does not fire on_changed");

    // read_only：不落字、退格无效
    TextInput ro;
    ro.set_value("abc").set_read_only(true);
    ro.on_focus_change(true);
    TextInputEvent e3;
    e3.text = "x";
    ro.on_text_input(e3);
    AURORA_TEST_CHECK_MSG(ro.value() == "abc", "TextInput: read_only ignores input");
    KeyEvent bk;
    bk.action = KeyAction::Down;
    bk.key = static_cast<int>(KeyCode::Backspace);
    ro.on_key_event(bk);
    AURORA_TEST_CHECK_MSG(ro.value() == "abc", "TextInput: read_only ignores backspace");

    // on_submit：Enter 触发
    TextInput si;
    std::string submitted;
    si.set_value("go").set_on_submit([&](const std::string &v) -> void { submitted = v; });
    si.on_focus_change(true);
    KeyEvent enter;
    enter.action = KeyAction::Down;
    enter.key = static_cast<int>(KeyCode::Enter);
    si.on_key_event(enter);
    AURORA_TEST_CHECK_MSG(submitted == "go", "TextInput: Enter fires on_submit");

    // 新样式/行为属性序列化往返；focused_border_color 未设置不输出（跟随主题）
    TextInput st;
    Json j0;
    st.serialize_props(j0);
    AURORA_TEST_CHECK_MSG(!j0.contains("focused_border_color"),
                          "TextInput: unset focused border color not serialized (follows theme)");

    st.set_text_color(Color{ 1, 2, 3, 255 })
        .set_background(Color{ 4, 5, 6, 255 })
        .set_border_color(Color{ 7, 8, 9, 255 })
        .set_focused_border_color(Color::red())
        .set_border_width(2.0f)
        .set_selection_color(Color{ 10, 11, 12, 90 })
        .set_max_length(7)
        .set_read_only(true)
        .set_obscure_text(true);
    Json j;
    st.serialize_props(j);
    TextInput rt;
    rt.deserialize_props(j);
    Json k;
    rt.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["text_color"][2].get<int>() == 3, "TextInput: text_color roundtrip");
    AURORA_TEST_CHECK_MSG(k["background"][0].get<int>() == 4, "TextInput: background roundtrip");
    AURORA_TEST_CHECK_MSG(k["focused_border_color"][0].get<int>() == 255, "TextInput: focused_border_color roundtrip");
    AURORA_TEST_CHECK_MSG(near_f(k["border_width"].get<float>(), 2.0f), "TextInput: border_width roundtrip");
    AURORA_TEST_CHECK_MSG(k["max_length"].get<int>() == 7, "TextInput: max_length roundtrip");
    AURORA_TEST_CHECK_MSG(k["read_only"].get<bool>() && k["obscure_text"].get<bool>(),
                          "TextInput: read_only/obscure roundtrip");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== text_input_test ===\n");
    test_chained_setters();
    test_serialize_roundtrip();
    test_defaults();
    test_selection_endpoint_highlight();
    test_behavior_props();
}
