/// 测试类型: unit
/// 目标单元: include/aurora/widget/button.h
/// 测试说明: 覆盖 Button——默认属性、label 与 on_click 回调（activate 与 Press/Release 合成点击）、
/// 禁用态不触发回调、min_size 布局与约束钳制、自描述元数据、属性序列化往返、
/// 标签 i18n（绘制 / 无障碍 / 自然宽同源，字面档输出不变）

#include <string>
#include <vector>

#include "aurora/environment/environment.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/string_table.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/widget/button.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_button {

using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 有效字号：与 `on_layout` / `paint_label` 的兜底口径一致（`size_pt` 非正时按 14）。
auto effective_font(const Button &b) -> Font {
    Font f = b.font;
    if (f.size_pt <= 0.0F) {
        f.size_pt = 14.0F;
    }
    return f;
}

#ifdef AURORA_ENABLE_DISPLAY_LIST
/// 录制-回放探针：把 Painter 录下的 DisplayList 回放到本后端，取出每条 DrawText 的文本与包围盒。
/// 走的是生产侧同一条 `DisplayList::replay` 通路（唯一实现），不新增任何观测专用 API。
class TextCapture final : public rhi::RhiBackend {
  public:
    struct Entry {
        std::string text;  ///< DrawText 落笔的显示串
        Rect bounds;  ///< 该次 draw_text 的包围盒（宽 = 实测字宽）
    };

    [[nodiscard]] auto name() const -> std::string_view override { return "utest_button_text_capture"; }

    auto submit(const DrawCmd &cmd, const rhi::CmdData &data) -> void override {
        if (cmd.kind == CmdKind::DrawText && data.text != nullptr) {
            entries.push_back(Entry{.text = *data.text, .bounds = cmd.bounds});
        }
    }

    // 探针后端：命令序列供用例直接读取，故有意保持公开。
    // NOLINTNEXTLINE(*-non-private-member-variables-in-classes)
    std::vector<Entry> entries;  ///< 按提交顺序记录的文本绘制
};

/// 画一遍并取回本帧发出的全部 DrawText（录制模式：只记命令、不上屏）。
auto capture_draw_text(Widget &w, const Rect &box, const BuildContext &ctx) -> std::vector<TextCapture::Entry> {
    Painter p;
    p.begin(static_cast<int>(box.size.width) + 8, static_cast<int>(box.size.height) + 8);
    DisplayList dl;
    p.record(dl);
    w.paint(p, box, ctx);
    p.stop();
    TextCapture cap;
    dl.replay(cap);
    return cap.entries;
}
#endif

/// 直绘一遍（非录制）并把整块画布读回，供逐像素比对。
auto render_to_pixels(Widget &w, const Rect &box, const BuildContext &ctx, int cw, int ch) -> std::vector<Color> {
    Painter p;
    p.begin(cw, ch);
    w.paint(p, box, ctx);
    std::vector<Color> px;
    px.reserve(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch));
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            px.push_back(p.get_pixel(x, y));
        }
    }
    return px;
}

/// 统计按钮盒内与背景色不同的像素数：圆角置 0 后盒内除文字外全是背景，故该数 > 0 等价于「文字真落了墨」。
/// 只扫盒内（盒外画布恒透明，计入会把判据变成恒真的空转）。
auto count_ink_outside_background(Widget &w, const Rect &box, const BuildContext &ctx, Color bg) -> int {
    Painter p;
    p.begin(static_cast<int>(box.size.width) + 8, static_cast<int>(box.size.height) + 8);
    w.paint(p, box, ctx);
    int ink = 0;
    for (int y = 0; y < static_cast<int>(box.size.height); ++y) {
        for (int x = 0; x < static_cast<int>(box.size.width); ++x) {
            if (p.get_pixel(x, y) != bg) {
                ++ink;
            }
        }
    }
    return ink;
}

/// 挑一个本平台字体真有字形的非 ASCII 译文。以「必然无字形的私用区码点 U+E000」为独立基准：
/// 量宽 > 0 且不等于该基准才算有字形；挑不出返回空串，由调用方记 SKIP（不给假绿）。
auto pick_non_ascii_translation(const Font &f) -> std::string {
    const std::string missing = "\xEE\x80\x80";  // U+E000（私用区）：任何常规字体都无字形
    const float missing_w = render::FontEngine::measure_width(missing, f);
    // CJK-LITERAL: cjk-fixture - 非 ASCII 译文候选，用于验证自然宽随译文变化重测
    const std::vector<std::string> candidates = {"保存", "Ελληνικά", "Sélectionner"};
    for (const auto &c : candidates) {
        const float w = render::FontEngine::measure_width(c, f);
        if (w > 0.0F && w != missing_w) {
            return c;
        }
    }
    return {};
}

}  // namespace

AURORA_TEST_CASE(button_defaults_and_type_name) {
    const Button b;
    AURORA_TEST_CHECK_EQ(std::string{b.type_name()}, "Button");
    const ButtonProps d = Button::defaults();
    AURORA_TEST_CHECK_TRUE(d.enabled);
    AURORA_TEST_CHECK_NEAR(d.corner_radius, 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_height, 0.0F, 1e-4F);
    // 默认背景色为 blue，文字色为 white。
    AURORA_TEST_CHECK_TRUE(d.color.get() == Color::blue());
    AURORA_TEST_CHECK_TRUE(d.on_color == Color::white());
}

AURORA_TEST_CASE(button_label_and_activate_fires_on_click) {
    int clicks = 0;
    Button b("OK");
    AURORA_TEST_CHECK_EQ(b.label.get().text, "OK");
    b.set_label("Confirm");
    AURORA_TEST_CHECK_EQ(b.label.get().text, "Confirm");

    b.set_on_click([&clicks]() -> void { ++clicks; });
    AURORA_TEST_CHECK_TRUE(b.wants_click());
    b.activate();
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 2);
}

AURORA_TEST_CASE(button_disabled_activate_does_not_fire) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });
    b.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(b.wants_click());
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 0);

    // 重新启用后恢复可点击。
    b.set_enabled(true);
    b.activate();
    AURORA_TEST_CHECK_EQ(clicks, 1);
}

AURORA_TEST_CASE(button_disabled_swallows_pointer_events) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });
    b.set_enabled(false);

    MouseEvent press;
    press.action = MouseAction::Press;
    b.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    b.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);
    AURORA_TEST_CHECK_EQ(clicks, 0);
}

AURORA_TEST_CASE(button_pointer_press_release_fires_click_once) {
    int clicks = 0;
    Button b("OK");
    b.set_on_click([&clicks]() -> void { ++clicks; });

    MouseEvent press;
    press.action = MouseAction::Press;
    b.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_EQ(clicks, 0);  // 按下不立即触发

    MouseEvent release;
    release.action = MouseAction::Release;
    b.on_pointer_event(release);
    AURORA_TEST_CHECK_EQ(clicks, 1);  // 完整按下-抬起触发一次
}

AURORA_TEST_CASE(button_layout_honors_min_size_and_clamps) {
    // 空标签 + 零内边距：自然尺寸为 0，被 min_size 撑到 (120, 48)。
    Button b("");
    b.set_padding(EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    b.set_min_size(120.0F, 48.0F);
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(b.size().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(b.size().height, 48.0F, 1e-4F);

    // 约束不足时钳制到 max。
    LayoutEngine::layout(b, bounded(50.0F, 20.0F));
    AURORA_TEST_CHECK_NEAR(b.size().width, 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(b.size().height, 20.0F, 1e-4F);

    // 默认内边距下任意标签宽度至少含左右 padding（12+12）。
    Button c("OK");
    LayoutEngine::layout(c, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_GE(c.size().width, 24.0F);
    AURORA_TEST_CHECK_GE(c.size().height, 12.0F);
}

AURORA_TEST_CASE(button_describe_reports_metadata) {
    const auto d = Button::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Button");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_click");
    bool has_label = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "label") {
            has_label = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_label);
}

AURORA_TEST_CASE(button_serialize_deserialize_roundtrip) {
    // CJK-LITERAL: cjk-fixture - Han label must survive the JSON props round-trip byte-for-byte
    Button src("确认");
    src.set_enabled(false);
    src.set_corner_radius(10.0F);
    src.set_min_size(80.0F, 36.0F);

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "label"),
                         "确认");  // CJK-LITERAL: cjk-fixture - Han label in JSON
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "enabled"), false);

    Button dst;
    dst.deserialize_props(props);
    // CJK-LITERAL: cjk-fixture - Han label restored unchanged after deserialization
    AURORA_TEST_CHECK_EQ(dst.label.get().text, "确认");
    AURORA_TEST_CHECK_FALSE(dst.enabled);
    AURORA_TEST_CHECK_NEAR(dst.corner_radius, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.min_width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.min_height, 36.0F, 1e-4F);
}

// ---- 标签 i18n：绘制 / 无障碍 / 自然宽同源，字面档输出不变 ----

AURORA_TEST_CASE(button_tr_label_draws_string_table_text) {
    auto &table = default_string_table();
    const std::string key = "utest_button.tr_label";
    const std::string translated = "Persist changes";
    table.add(Locale{}, key, translated);

    // tr() 构造时 text 恒空、只带 key：修法前 paint_label 直读 label.get().text，画出来是空白。
    Button b;
    b.label = LocalizedString::tr(key);
    AURORA_TEST_CHECK_TRUE(b.label.get().text.empty());

    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    const BuildContext ctx;  // 无 Provider<Locale> 注入 → 回落 default_string_table() 的缺省档
    const Rect box{.origin = Point{}, .size = b.size()};

#ifdef AURORA_ENABLE_DISPLAY_LIST
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, translated);
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_DISPLAY_LIST is off; draw-record probe unavailable");
#endif

    // 有墨：圆角置 0 后盒内除文字外全是背景色，非背景像素数 > 0 即文字真的落了笔。
    Button inked;
    inked.label = LocalizedString::tr(key);
    inked.set_corner_radius(0.0F);
    LayoutEngine::layout(inked, bounded(300.0F, 80.0F));
    const Rect ink_box{.origin = Point{}, .size = inked.size()};
    AURORA_TEST_CHECK_GT(count_ink_outside_background(inked, ink_box, ctx, inked.color.get()), 0);
}

AURORA_TEST_CASE(button_accessibility_label_equals_drawn_text) {
    auto &table = default_string_table();
    const std::string key = "utest_button.a11y";
    table.add(Locale{}, key, "Apply changes");

    Button b;
    b.label = LocalizedString::tr(key);
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    const BuildContext ctx;
    const Rect box{.origin = Point{}, .size = b.size()};

#ifdef AURORA_ENABLE_DISPLAY_LIST
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    // 预期取绘制侧同一条 resolved_label() 的产物，不在用例里另算一遍。
    AURORA_TEST_CHECK_EQ(b.accessibility_label(), entries[0].text);
    AURORA_TEST_CHECK_EQ(b.accessibility_label(), "Apply changes");
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_DISPLAY_LIST is off; draw-record probe unavailable");
#endif
}

AURORA_TEST_CASE(button_natural_width_equals_resolved_label_width_plus_padding) {
    auto &table = default_string_table();
    const std::string key = "utest_button.width";
    table.add(Locale{}, key, "Short");
    table.add(Locale{.language = "zz"}, key, "Considerably longer caption");

    Button b;
    b.label = LocalizedString::tr(key);
    b.set_min_size(0.0F, 0.0F);
    const Font f = effective_font(b);

    // 缺省档（无 Locale 注入）：自然宽 = 显示串量宽 + 左右内边距。
    LayoutEngine::layout(b, bounded(400.0F, 80.0F));
    const float expected_en = render::FontEngine::measure_width("Short", f) + b.padding.left + b.padding.right;
    AURORA_TEST_CHECK_NEAR(b.size().width, expected_en, 1e-3F);

    // 注入 Locale 后重新布局：必须按新译文重测，而不是复用缺省档的旧宽。
    const Environment env = Environment{}.with<Locale>(Locale{.language = "zz"});
    BuildContext ctx;
    ctx.env = &env;
    b.mark_needs_layout();  // 约束与上一轮相同，布局缓存会命中并跳过 on_layout
    LayoutEngine::layout(b, bounded(400.0F, 80.0F), ctx);
    const float expected_zz =
        render::FontEngine::measure_width("Considerably longer caption", f) + b.padding.left + b.padding.right;
    AURORA_TEST_CHECK_NEAR(b.size().width, expected_zz, 1e-3F);
    AURORA_TEST_CHECK_GT(expected_zz, expected_en);  // 两条期望值有区分度，判据非空转
}

AURORA_TEST_CASE(button_natural_width_covers_non_ascii_translation) {
    auto &table = default_string_table();
    const std::string key = "utest_button.non_ascii";

    Button b;
    b.label = LocalizedString::tr(key);
    b.set_min_size(0.0F, 0.0F);
    const Font f = effective_font(b);

    const std::string translated = pick_non_ascii_translation(f);
    if (translated.empty()) {
        AURORA_TEST_SKIP("no non-ASCII candidate has real glyph coverage in this platform's font face");
    }
    table.add(Locale{}, key, translated);
    LayoutEngine::layout(b, bounded(400.0F, 80.0F));
    const float expected = render::FontEngine::measure_width(translated, f) + b.padding.left + b.padding.right;
    AURORA_TEST_CHECK_NEAR(b.size().width, expected, 1e-3F);
    AURORA_TEST_CHECK_GT(expected, b.padding.left + b.padding.right);  // 译文确实量出了宽度
}

AURORA_TEST_CASE(button_paint_remeasures_when_display_text_changes_without_relayout) {
    auto &table = default_string_table();
    const std::string key = "utest_button.cache";
    table.add(Locale{}, key, "Old");

    Button b;
    b.label = LocalizedString::tr(key);
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));  // 缓存落 "Old" 的量宽

    table.add(Locale{}, key, "Substantially longer");  // 显示串变了，但刻意不重排
    const BuildContext ctx;
    const Rect box{.origin = Point{}, .size = b.size()};
#ifdef AURORA_ENABLE_DISPLAY_LIST
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, "Substantially longer");
    const Font f = effective_font(b);
    const float old_w = render::FontEngine::measure_width("Old", f);
    const float fresh_w = render::FontEngine::measure_width("Substantially longer", f);
    AURORA_TEST_CHECK_GT(fresh_w, old_w);
    // 缓存失效腿：显示串与上次测量用串不同 → 重测，不能复用 "Old" 的旧宽。
    AURORA_TEST_CHECK_NEAR(entries[0].bounds.size.width, fresh_w, 1e-3F);
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_DISPLAY_LIST is off; draw-record probe unavailable");
#endif
}

AURORA_TEST_CASE(button_literal_label_output_is_unchanged) {
    auto &table = default_string_table();
    const std::string key = "utest_button.literal";
    table.add(Locale{}, key, "Save");  // 表值恰好等于字面串

    Button literal("Save");
    Button via_table;
    via_table.label = LocalizedString::tr(key);

    LayoutEngine::layout(literal, bounded(300.0F, 80.0F));
    LayoutEngine::layout(via_table, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(literal.size().width, via_table.size().width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(literal.size().height, via_table.size().height, 1e-4F);

    const BuildContext ctx;
    const Rect box{.origin = Point{}, .size = literal.size()};

#ifdef AURORA_ENABLE_DISPLAY_LIST
    // 字面档落笔的串仍是原文本，包围盒宽度也仍是它的量宽。
    const auto entries = capture_draw_text(literal, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, "Save");
    AURORA_TEST_CHECK_NEAR(entries[0].bounds.size.width,
                           render::FontEngine::measure_width("Save", effective_font(literal)), 1e-3F);
#else
    AURORA_TEST_SKIP("AURORA_ENABLE_DISPLAY_LIST is off; draw-record probe unavailable");
#endif

    // 帧缓冲自证：字面档与「解析后同串」档必须逐像素同值——字面路径不走查表时就是这个输出。
    const auto px_literal = render_to_pixels(literal, box, ctx, 200, 60);
    const auto px_via_table = render_to_pixels(via_table, box, ctx, 200, 60);
    AURORA_TEST_REQUIRE_EQ(px_literal.size(), px_via_table.size());
    AURORA_TEST_CHECK_TRUE(px_literal == px_via_table);
}

AURORA_TEST_CASE(cursor_shape_hook_defaults_to_pointing_hand) {
    // Button 悬停默认 PointingHand（控件级虚钩子）；修饰链显式声明在派发器解析时优先。
    Button b;
    AURORA_TEST_CHECK(b.cursor_shape() == std::optional{CursorShape::PointingHand});
}

}  // namespace aurora::test_cases::utest_button
