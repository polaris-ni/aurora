/// 测试类型: unit
/// 目标单元: include/aurora/widget/button.h
/// 测试说明: 覆盖 Button——默认属性与表态位、label 与 on_click 回调（activate 与 Press/Release 合成点击）、
/// 禁用态不触发回调、min_size 布局与约束钳制、自描述元数据、属性序列化往返、
/// 标签 i18n（绘制 / 无障碍 / 自然宽同源，字面档输出不变）、主题跟随（未表态时取主题角色
/// 与 `shapes` / `spacing` 档，含风格轴；显式表态优先）。属性单一声明迁移后另覆盖权威表的读消费面：
/// 挂载后逐档标脏、与令牌同值的显式表态不随换肤漂移、撤表态重新随档且该键退出序列化、
/// 常量档读件在两档主题下与库内解析全同、Inspector 单键热回写写穿当前源而不断绑、
/// 全量表态的 JSON 逐行复原生效值并落同一帧像素、非法 JSON 逐档降级（可空行撤表态退回回落点、
/// 必填行保留现值、降级经 Diagnostics 上报）

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/environment/environment.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/message_catalog.h"
#include "aurora/i18n/message_id.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/display_list.h"
#include "aurora/render/rhi/rhi_backend.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/inspect.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_button {

// 测试词表：基址 901000（每文件独立区间，避免同进程测试桶内区间撞车）；
// 名字与基址在此一次性登记，default_catalog 为进程级单例。
inline constexpr aurora::MessageId AURORA_UTEST_TR_LABEL{901000};
inline constexpr aurora::MessageId AURORA_UTEST_A11Y{901001};
inline constexpr aurora::MessageId AURORA_UTEST_WIDTH{901002};
inline constexpr aurora::MessageId AURORA_UTEST_NON_ASCII{901003};
inline constexpr aurora::MessageId AURORA_UTEST_CACHE{901004};
inline constexpr aurora::MessageId AURORA_UTEST_LITERAL{901005};

auto ensure_button_vocab() -> void {
    static const bool ONCE = [] {
        (void)aurora::default_catalog().register_vocabulary(
            901000, {"utest_button.tr_label", "utest_button.a11y", "utest_button.width", "utest_button.non_ascii",
                     "utest_button.cache", "utest_button.literal"});
        return true;
    }();
    (void)ONCE;
}

using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 有效字号：与 `on_layout` / `paint_label` 的兜底口径一致（`font_size` 行未表态或表态非正时按 14）。
/// 本件只从公共读件取值，不再直读字段——迁移后 `Button` 已无聚合 `font` 可读。
auto probe_font(const Button &b) -> Font {
    const float stated_size = b.font_size();
    return Font{.size_pt = stated_size > 0.0F ? stated_size : 14.0F};
}

/// 探针：把只在库内 / 控件内部消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析、
/// `collect_signals` 信号采集，以及 `Button` 自己的两枚状态色解析钩子）。四者都不升公共面，
/// 故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class PropsProbe final : public Button {
  public:
    using Button::collect_signals;
    using Button::present;
    using Button::resolve;
    using Button::resolve_background;
    using Button::resolve_text_color;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
/// 用现成的公共观察件而不是新加接口：`mark_needs_layout()` 走 `request_frame(true)`、
/// `mark_needs_paint()` 走 `request_frame(false)`，回调直接挂在本控件上即命中（无父可溯时 `top == this`）。
struct ButtonDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（返回时先落零，用例只断增量——挂载与重订阅会回放一轮双档脏）。
    /// @param target 待观察的控件。
    auto attach_to(Button &target) -> void {
        paints = 0;
        layouts = 0;
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

/// @brief 「这行能不能撤表态」的可检测性：必填行必须在 `clear` 的重载集里根本没有这个名字。
template <typename Row>
concept button_row_clearable = requires(ButtonProps props, Row row) { props.clear(row); };

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

/// 直绘一遍（非录制）并把整块画布读回，供逐像素比对。
[[maybe_unused]] auto render_to_pixels(Widget &w, const Rect &box, const BuildContext &ctx, int cw, int ch)
    -> std::vector<Color> {
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
    const Button bare;
    AURORA_TEST_CHECK_EQ(std::string{bare.type_name()}, "Button");
    // `defaults()` 就是「一行都没表态」那份属性对象：读面照常给生效值，表态位全为假。
    const ButtonProps d = Button::defaults();
    AURORA_TEST_CHECK_TRUE(d.enabled());
    AURORA_TEST_CHECK_NEAR(d.font_size(), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_width(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.min_height(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(d.label().text.empty());
    // 圆角与内边距的「默认值」不再是本件字面量，而是「未表态」这一态：取值随主题形状 / 间距档
    // 解算（Material 基线下实测为 6 与 12/6，由 button_radius_and_padding_follow_the_theme 逐档核）。
    PropsProbe probe;
    AURORA_TEST_CHECK_FALSE(probe.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_FALSE(probe.present(ButtonProps::padding_row()));
    // 背景 / 文字色同样未表态：跟随主题角色（roles.primary / roles.on_primary），换肤即生效。
    AURORA_TEST_CHECK_FALSE(probe.present(ButtonProps::color_row()));
    AURORA_TEST_CHECK_FALSE(probe.present(ButtonProps::on_color_row()));
    // 必填行没有可撤的表态位：`clear(label_row())` 在重载集里根本不存在（编译期拒绝，不是运行期判空）。
    static_assert(!button_row_clearable<decltype(ButtonProps::label_row())>);
    static_assert(button_row_clearable<decltype(ButtonProps::corner_radius_row())>);
}

AURORA_TEST_CASE(button_label_and_activate_fires_on_click) {
    int clicks = 0;
    Button b("OK");
    AURORA_TEST_CHECK_EQ(b.label().text, "OK");
    b.set_label("Confirm");
    AURORA_TEST_CHECK_EQ(b.label().text, "Confirm");

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
    b.set_min_width(120.0F).set_min_height(48.0F);
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
    src.set_min_width(80.0F).set_min_height(36.0F);

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "label"),
                         "确认");  // CJK-LITERAL: cjk-fixture - Han label in JSON
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "enabled"), false);

    PropsProbe dst;
    dst.deserialize_props(props);
    // CJK-LITERAL: cjk-fixture - Han label restored unchanged after deserialization
    AURORA_TEST_CHECK_EQ(dst.label().text, "确认");
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    // 表过态即随键往返，不被主题档吞掉：表态位与生效值两处都核。
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_NEAR(dst.corner_radius(BuildContext{}), 10.0F, 1e-4F);
    // 未随键来的行保持未表态（写盘的是意图集合，回填不会把系统默认伪装成用户意图）。
    AURORA_TEST_CHECK_FALSE(dst.present(ButtonProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(dst.min_width(), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.min_height(), 36.0F, 1e-4F);
}

// ---- 标签 i18n：绘制 / 无障碍 / 自然宽同源，字面档输出不变 ----

AURORA_TEST_CASE(button_tr_label_draws_catalog_text) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_TR_LABEL;
    const std::string key_name = "utest_button.tr_label";
    const std::string translated = "Persist changes";
    table.add(Locale{}, key_name, translated);

    // tr() 构造时 text 恒空、只带 key：修法前 paint_label 直读 label.text，画出来是空白。
    Button b;
    b.set_label(LocalizedString::tr(key));
    AURORA_TEST_CHECK_TRUE(b.label().text.empty());

    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    const BuildContext ctx;  // 无 Provider<Locale> 注入 → 回落 default_catalog() 的缺省档

    const Rect box{.origin = Point{}, .size = b.size()};
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, translated);

    // 有墨：圆角置 0 后盒内除文字外全是背景色，非背景像素数 > 0 即文字真的落了笔。
    Button inked;
    inked.set_label(LocalizedString::tr(key));
    inked.set_corner_radius(0.0F);
    inked.set_color(Color::blue());  // 显式表态：不依赖主题回落，背景参数与之一致
    LayoutEngine::layout(inked, bounded(300.0F, 80.0F));
    const Rect ink_box{.origin = Point{}, .size = inked.size()};
    AURORA_TEST_CHECK_GT(count_ink_outside_background(inked, ink_box, ctx, Color::blue()), 0);
}

AURORA_TEST_CASE(button_accessibility_label_equals_drawn_text) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_A11Y;
    const std::string key_name = "utest_button.a11y";
    table.add(Locale{}, key_name, "Apply changes");

    Button b;
    b.set_label(LocalizedString::tr(key));
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));
    const BuildContext ctx;

    const Rect box{.origin = Point{}, .size = b.size()};
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    // 预期取绘制侧同一条 resolved_label() 的产物，不在用例里另算一遍。
    AURORA_TEST_CHECK_EQ(b.accessibility_label(), entries[0].text);
    AURORA_TEST_CHECK_EQ(b.accessibility_label(), "Apply changes");
}

AURORA_TEST_CASE(button_natural_width_equals_resolved_label_width_plus_padding) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_WIDTH;
    const std::string key_name = "utest_button.width";
    table.add(Locale{}, key_name, "Short");
    table.add(Locale{.language = "zz"}, key_name, "Considerably longer caption");

    Button b;
    b.set_label(LocalizedString::tr(key));
    const Font f = probe_font(b);

    // 缺省档（无 Locale 注入）：自然宽 = 显示串量宽 + 左右内边距。
    LayoutEngine::layout(b, bounded(400.0F, 80.0F));
    const float expected_en =
        render::FontEngine::measure_width("Short", f) + (aurora::Theme::light().spacing.large * 2.0F);
    AURORA_TEST_CHECK_NEAR(b.size().width, expected_en, 1e-3F);

    // 注入 Locale 后重新布局：必须按新译文重测，而不是复用缺省档的旧宽。
    const Environment env = Environment{}.with<Locale>(Locale{.language = "zz"});
    BuildContext ctx;
    ctx.env = &env;
    b.mark_needs_layout();  // 约束与上一轮相同，布局缓存会命中并跳过 on_layout
    LayoutEngine::layout(b, bounded(400.0F, 80.0F), ctx);
    const float expected_zz = render::FontEngine::measure_width("Considerably longer caption", f) +
                              (aurora::Theme::light().spacing.large * 2.0F);
    AURORA_TEST_CHECK_NEAR(b.size().width, expected_zz, 1e-3F);
    AURORA_TEST_CHECK_GT(expected_zz, expected_en);  // 两条期望值有区分度，判据非空转
}

AURORA_TEST_CASE(button_natural_width_covers_non_ascii_translation) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_NON_ASCII;
    const std::string key_name = "utest_button.non_ascii";

    Button b;
    b.set_label(LocalizedString::tr(key));
    const Font f = probe_font(b);

    const std::string translated = pick_non_ascii_translation(f);
    if (translated.empty()) {
        AURORA_TEST_SKIP("no non-ASCII candidate has real glyph coverage in this platform's font face");
    }
    table.add(Locale{}, key_name, translated);
    LayoutEngine::layout(b, bounded(400.0F, 80.0F));
    const float expected =
        render::FontEngine::measure_width(translated, f) + (aurora::Theme::light().spacing.large * 2.0F);
    AURORA_TEST_CHECK_NEAR(b.size().width, expected, 1e-3F);
    AURORA_TEST_CHECK_GT(expected, aurora::Theme::light().spacing.large * 2.0F);  // 译文确实量出了宽度
}

AURORA_TEST_CASE(button_paint_remeasures_when_display_text_changes_without_relayout) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_CACHE;
    const std::string key_name = "utest_button.cache";
    table.add(Locale{}, key_name, "Old");

    Button b;
    b.set_label(LocalizedString::tr(key));
    LayoutEngine::layout(b, bounded(300.0F, 80.0F));  // 缓存落 "Old" 的量宽

    table.add(Locale{}, key_name, "Substantially longer");  // 显示串变了，但刻意不重排
    const BuildContext ctx;
    const Rect box{.origin = Point{}, .size = b.size()};
    const auto entries = capture_draw_text(b, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, "Substantially longer");
    const Font f = probe_font(b);
    const float old_w = render::FontEngine::measure_width("Old", f);
    const float fresh_w = render::FontEngine::measure_width("Substantially longer", f);
    AURORA_TEST_CHECK_GT(fresh_w, old_w);
    // 缓存失效路径：显示串与上次测量用串不同 → 重测，不能复用 "Old" 的旧宽。
    AURORA_TEST_CHECK_NEAR(entries[0].bounds.size.width, fresh_w, 1e-3F);
}

AURORA_TEST_CASE(button_literal_label_output_is_unchanged) {
    ensure_button_vocab();
    auto &table = aurora::default_catalog();
    const auto key = AURORA_UTEST_LITERAL;
    const std::string key_name = "utest_button.literal";
    table.add(Locale{}, key_name, "Save");  // 表值恰好等于字面串

    Button literal("Save");
    Button via_table;
    via_table.set_label(LocalizedString::tr(key));

    LayoutEngine::layout(literal, bounded(300.0F, 80.0F));
    LayoutEngine::layout(via_table, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(literal.size().width, via_table.size().width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(literal.size().height, via_table.size().height, 1e-4F);

    const BuildContext ctx;

    const Rect box{.origin = Point{}, .size = literal.size()};
    // 字面档落笔的串仍是原文本，包围盒宽度也仍是它的量宽。
    const auto entries = capture_draw_text(literal, box, ctx);
    AURORA_TEST_REQUIRE_FALSE(entries.empty());
    AURORA_TEST_CHECK_EQ(entries[0].text, "Save");
    AURORA_TEST_CHECK_NEAR(entries[0].bounds.size.width, render::FontEngine::measure_width("Save", probe_font(literal)),
                           1e-3F);
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

// ---- 主题跟随：未表态 → 角色回落；显式表态 → 不被主题覆盖 ----

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用
/// （调用方持有 holder 即保证环境存活；BuildContext 本就只供当帧读取）。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

AURORA_TEST_CASE(button_colors_follow_theme_when_unset) {
    // 两态语义：未表态时取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    PropsProbe b;
    AURORA_TEST_CHECK(b.resolve_background(bare) == Color::blue());  // light primary = (0,0,255)
    AURORA_TEST_CHECK(b.resolve_text_color(bare) == Color::white());  // light on_primary

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(b.resolve_background(dark) == Color(0xC0, 0xC1, 0xFF));  // dark primary（WCAG 修正对）
    AURORA_TEST_CHECK(b.resolve_text_color(dark) == Color(0x00, 0x2B, 0x73));  // dark on_primary
}

AURORA_TEST_CASE(button_explicit_colors_override_theme) {
    // 对照组：显式表态不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    PropsProbe b;
    b.set_color(Color{0xE0, 0x1B, 0x24});  // error 红，刻意区别于两档 primary
    b.set_on_color(Color{0x12, 0x34, 0x56});
    AURORA_TEST_CHECK(b.resolve_background(dark) == Color(0xE0, 0x1B, 0x24));
    AURORA_TEST_CHECK(b.resolve_text_color(dark) == Color(0x12, 0x34, 0x56));
}

// ---- 度量跟随：未表态 → 读主题形状 / 间距档；表过态 → 主题不覆盖 ----

AURORA_TEST_CASE(button_metrics_default_to_material_baseline) {
    // 无注入 = Material 基线档：迁移前后实测同为圆角 6、内边距 12/6，观感零回归的锚点。
    const BuildContext bare;
    PropsProbe b;
    AURORA_TEST_CHECK_NEAR(b.corner_radius(bare), 6.0F, 1e-4F);
    const EdgeInsets pad = b.padding(bare);
    AURORA_TEST_CHECK_NEAR(pad.left, 12.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.right, 12.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.top, 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.bottom, 6.0F, 1e-4F);
}

AURORA_TEST_CASE(button_metrics_follow_theme_tokens_when_unset) {
    // 改过令牌的主题 ⇒ 生效值随令牌走（证明读的是 theme，而不是写死的常量）。
    Theme t = Theme::light();
    t.shapes.small = 11.5F;
    t.spacing.large = 20.0F;
    t.spacing.small = 7.0F;
    const auto [env, ctx] = themed_ctx(t);
    PropsProbe b;
    AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx), 11.5F, 1e-4F);
    const EdgeInsets pad = b.padding(ctx);
    AURORA_TEST_CHECK_NEAR(pad.left, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.right, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.top, 7.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.bottom, 7.0F, 1e-4F);
}

AURORA_TEST_CASE(button_metrics_follow_style_arm) {
    // 两轴的风格轴同样驱动度量：Windows/Gnome/Kde 的 `shapes.small` 与基线档实测不同值，
    // 若解析仍回 6（未接线）本用例即红——四档取值互异，同时钉住「不是巧合相等」。
    struct Arm {
        ThemeStyle style;
        float expected;
    };
    static constexpr Arm ARMS[] = {
        {.style = ThemeStyle::Material, .expected = 6.0F},
        {.style = ThemeStyle::Windows, .expected = 4.0F},
        {.style = ThemeStyle::Gnome, .expected = 9.0F},
        {.style = ThemeStyle::Kde, .expected = 5.0F},
    };
    PropsProbe b;
    for (const Arm &arm : ARMS) {
        const auto [env, ctx] = themed_ctx(Theme::from_style(arm.style, ColorScheme::Light));
        AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx), arm.expected, 1e-4F);
    }
}

AURORA_TEST_CASE(button_explicit_metrics_override_theme) {
    // 对照组：显式表过态的圆角 / 内边距，换任何主题档都不动（两态语义的另一半）。
    Theme t = Theme::light();
    t.shapes.small = 11.5F;
    t.spacing.large = 20.0F;
    t.spacing.small = 7.0F;
    const auto [env, ctx] = themed_ctx(t);
    PropsProbe b;
    b.set_corner_radius(0.0F);
    b.set_padding(EdgeInsets{.left = 3.0F, .top = 2.0F, .right = 3.0F, .bottom = 2.0F});
    AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx), 0.0F, 1e-4F);
    const EdgeInsets pad = b.padding(ctx);
    AURORA_TEST_CHECK_NEAR(pad.left, 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(pad.top, 2.0F, 1e-4F);
}

// ===========================================================================
// 权威表的读消费面：挂载后逐档标脏 / 同值守卫 / 撤表态 / 窄面恒等 / 热回写不断绑 / 全量键往返
// ===========================================================================

AURORA_TEST_CASE(button_mounted_paint_tier_setter_requests_repaint_with_new_pixels) {
    Button b("OK");
    b.set_corner_radius(0.0F);  // 直角：整盒可作背景取样点，像素判据不受圆角影响
    const BuildContext ctx;
    b.mount(ctx);  // 先挂载：信号采集与订阅已建立，此后 setter 必须命中宿主
    ButtonDirty dirty;
    dirty.attach_to(b);
    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;

    const Color next{0x12, 0x34, 0x56, 0xFF};
    b.set_color(next);  // 绘制档行（回落点 = theme.primary），且是来源行
    // 来源行写入会先重订阅一次（「落码事实」其十五：每条订阅登记时就地首跑一轮双档脏，
    // 故落账绝对值 = 当时的来源数 + 本行那一笔），断**两档差额**才与来源数无关：
    // 差额恰为 1 即「本行的绘制档落了、且没有多落布局档」；漏标则差额为 0。
    AURORA_TEST_CHECK_EQ(dirty.paints - dirty.layouts, (paints0 - layouts0) + 1);
    AURORA_TEST_CHECK(b.color(ctx) == next);

    // 「取到新画面」：落笔的背景像素真的换了色，不是只记了一笔脏。
    LayoutEngine::layout(b, bounded(200.0F, 60.0F), ctx);
    const auto px = render_to_pixels(b, Rect{.origin = Point{}, .size = b.size()}, ctx, 200, 60);
    AURORA_TEST_REQUIRE_FALSE(px.empty());
    AURORA_TEST_CHECK(px[0] == next);

    const int paints_after = dirty.paints;
    const int layouts_after = dirty.layouts;
    b.set_color(next);  // 同值再写：判等短路挡住，不落账也不重绘（既不落本行那笔、也不重订阅）
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after);
}

AURORA_TEST_CASE(button_mounted_layout_tier_setter_relays_out_to_new_size) {
    Button b("");  // 空标签：自然尺寸与 min 档分离，落位判据只反映 min
    const BuildContext ctx;
    b.mount(ctx);
    ButtonDirty dirty;
    dirty.attach_to(b);

    b.set_padding(EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});  // 双档行
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    b.set_min_width(120.0F).set_min_height(48.0F);  // 两枚纯布局行：各落一次布局档
    AURORA_TEST_CHECK_EQ(dirty.layouts, 3);
    // 非来源行不触发重订阅，落账即本行档位：`padding` 行声明了 `.layout().paint()`，
    // 故绘制档在这一笔里落 1 次，其后两枚布局行不再补绘制档。
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);

    LayoutEngine::layout(b, bounded(300.0F, 80.0F), ctx);
    AURORA_TEST_CHECK_NEAR(b.size().width, 120.0F, 1e-4F);  // 重排并按新尺寸落位
    AURORA_TEST_CHECK_NEAR(b.size().height, 48.0F, 1e-4F);

    const int layouts_before = dirty.layouts;
    const int paints_before = dirty.paints;
    b.set_font_size(20.0F);  // 双档行（layout + paint）：两档各落一次
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before + 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before + 1);
}

AURORA_TEST_CASE(button_stated_value_equal_to_token_does_not_follow_reskin) {
    // 同值守卫的两半：与当时令牌**同值**的显式表态是用户意图，换肤后保持；未表态的同一件随档而变。
    const float baseline_small = Theme::light().shapes.small;
    const BuildContext light;  // 无注入即回落 light，与 `Theme::light()` 同档

    PropsProbe stated;
    stated.set_corner_radius(baseline_small);  // 数值上与令牌相同，但表过态
    PropsProbe unstated;  // 同一枚取值，来自回落

    Theme tuned = Theme::light();
    tuned.shapes.small = baseline_small + 5.0F;
    const auto [env, ctx] = themed_ctx(tuned);

    AURORA_TEST_CHECK_NEAR(stated.corner_radius(light), baseline_small, 1e-4F);
    AURORA_TEST_CHECK_NEAR(unstated.corner_radius(light), baseline_small, 1e-4F);
    // 换肤后两半必须分化，否则「系统默认」与「用户默认」就合并成了。
    AURORA_TEST_CHECK_NEAR(stated.corner_radius(ctx), baseline_small, 1e-4F);
    AURORA_TEST_CHECK_NEAR(unstated.corner_radius(ctx), baseline_small + 5.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(stated.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_FALSE(unstated.present(ButtonProps::corner_radius_row()));
}

AURORA_TEST_CASE(button_clear_unstates_the_row_and_drops_its_key) {
    Theme tuned = Theme::light();
    tuned.shapes.small = 13.0F;
    const auto [env, ctx] = themed_ctx(tuned);

    PropsProbe b;
    b.set_corner_radius(2.0F);
    AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx), 2.0F, 1e-4F);
    Json stated = Json::object();
    b.serialize_props(stated);
    AURORA_TEST_CHECK_TRUE(stated.contains("corner_radius"));

    // 撤表态走公共面上唯一的动作型通用件（形参是带类型的行，必填行传进来编译即红）。
    b.clear(ButtonProps::corner_radius_row());
    AURORA_TEST_CHECK_FALSE(b.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx), 13.0F, 1e-4F);  // 重新随档
    Json cleared = Json::object();
    b.serialize_props(cleared);
    AURORA_TEST_CHECK_FALSE(cleared.contains("corner_radius"));  // 意图集合里再无此键

    // 同值守卫的逆命题：撤表态后换肤再度生效，说明撤的是表态位而不是把当时的值烘进字段。
    Theme again = Theme::light();
    again.shapes.small = 7.0F;
    const auto [env2, ctx2] = themed_ctx(again);
    AURORA_TEST_CHECK_NEAR(b.corner_radius(ctx2), 7.0F, 1e-4F);
}

AURORA_TEST_CASE(button_constant_rows_read_identically_under_light_and_dark) {
    // 恒等关系的正面取证：常量档行没有 `x(ctx)` 第二件，其无参读件**就是**生效值——
    // 明 / 暗两档主题下与库内 `resolve(row, ctx)` 取值全同，缺这组断言则「窄面不丢语义」只是断言。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    PropsProbe b;

    AURORA_TEST_CHECK(b.enabled() == b.resolve(ButtonProps::enabled_row(), light));
    AURORA_TEST_CHECK(b.enabled() == b.resolve(ButtonProps::enabled_row(), dark));
    AURORA_TEST_CHECK_NEAR(b.font_size(), b.resolve(ButtonProps::font_size_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(b.font_size(), b.resolve(ButtonProps::font_size_row(), dark), 1e-6F);
    AURORA_TEST_CHECK(b.border_color() == b.resolve(ButtonProps::border_color_row(), light));
    AURORA_TEST_CHECK(b.border_color() == b.resolve(ButtonProps::border_color_row(), dark));
    AURORA_TEST_CHECK_NEAR(b.border_width(), b.resolve(ButtonProps::border_width_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(b.border_width(), b.resolve(ButtonProps::border_width_row(), dark), 1e-6F);
    AURORA_TEST_CHECK(b.disabled_color() == b.resolve(ButtonProps::disabled_color_row(), light));
    AURORA_TEST_CHECK(b.disabled_color() == b.resolve(ButtonProps::disabled_color_row(), dark));
    AURORA_TEST_CHECK(b.disabled_text_color() == b.resolve(ButtonProps::disabled_text_color_row(), light));
    AURORA_TEST_CHECK(b.disabled_text_color() == b.resolve(ButtonProps::disabled_text_color_row(), dark));
    AURORA_TEST_CHECK_NEAR(b.min_width(), b.resolve(ButtonProps::min_width_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(b.min_height(), b.resolve(ButtonProps::min_height_row(), dark), 1e-6F);
    // 表态后仍与解析同值（两半都恒等，才排除「无参读件读的是另一份存储」）。
    b.set_min_width(64.0F);
    AURORA_TEST_CHECK_NEAR(b.min_width(), b.resolve(ButtonProps::min_width_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(b.min_width(), 64.0F, 1e-6F);
}

AURORA_TEST_CASE(button_serialize_emits_required_plus_stated_only) {
    // 未表态的非必填属性一律不写盘：序列化输出是意图集合，键数下降属预期。
    Button b;
    Json j = Json::object();
    b.serialize_props(j);
    AURORA_TEST_CHECK_TRUE(j.contains("label"));  // 必填行恒写
    for (const char *key :
         {"color", "on_color", "font_size", "corner_radius", "padding", "enabled", "hover_color", "pressed_color",
          "border_color", "border_width", "disabled_color", "disabled_text_color", "min_width", "min_height"}) {
        AURORA_TEST_CHECK_FALSE(j.contains(key));
    }

    // 表一行只多一键，且与回落基线的取值互不污染。
    Button c;
    c.set_corner_radius(3.0F);
    Json cj = Json::object();
    c.serialize_props(cj);
    AURORA_TEST_CHECK_TRUE(cj.contains("corner_radius"));
    AURORA_TEST_CHECK_FALSE(cj.contains("min_width"));
}

AURORA_TEST_CASE(button_inspector_hot_write_writes_through_the_bound_source) {
    // 绑外部源后经 Inspector 单键热回写：变更落回**当前源**（不是换绑常量镜像），订阅面不变。
    const State<Color> owner{Color{0x10, 0x20, 0x30, 0xFF}};

    // 来源集合的对照基准：未表色的空白按钮也有一枚来源 —— 必填行 `label` 本身就是 `ReadOnlyState`。
    // 本件因此不能按「1 枚」断言，只能按「比空白基线多一枚 `color`」断言。
    StateSink baseline_sink;
    PropsProbe bare;
    bare.collect_signals(baseline_sink);
    const std::size_t baseline_sources = baseline_sink.sources().size();

    PropsProbe b;
    b.set_color(owner);  // 同名写件的句柄重载（与值重载同名，`_source` 后缀已退役）
    b.mount(BuildContext{});
    ButtonDirty dirty;
    dirty.attach_to(b);
    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;

    auto collect_ids = [](PropsProbe &w) {
        StateSink sink;
        w.collect_signals(sink);
        std::vector<std::uint64_t> ids;
        for (const auto &node : sink.sources()) {
            ids.push_back(node->id());
        }
        std::sort(ids.begin(), ids.end());  // 只比集合，不比采集次序
        return std::pair{sink.sources().size(), ids};
    };

    const auto [count_before, ids_before] = collect_ids(b);
    AURORA_TEST_REQUIRE_EQ(count_before, baseline_sources + 1);  // 恰多一枚：外部 `color` 源

    // 取一帧与序列化同型的载荷（同一枚编解码登记，不在用例里手写颜色 JSON 形态）。
    Json carrier = Json::object();
    Button codec;
    codec.set_color(Color{0xAA, 0xBB, 0xCC, 0xFF});
    codec.serialize_props(carrier);
    AURORA_TEST_REQUIRE_TRUE(carrier.contains("color"));
    set_widget_prop(b, "color", *carrier.at("color"));  // Inspector 的单属性回写通道

    AURORA_TEST_CHECK(owner.get() == Color{0xAA, 0xBB, 0xCC, 0xFF});  // 外部源收到变更
    AURORA_TEST_CHECK(b.color(BuildContext{}) == Color{0xAA, 0xBB, 0xCC, 0xFF});
    // 热回写走**写穿当前源**这条路（`props_deserialize` 对已绑来源行调 `write_through`、不调 `assign`），
    // 故「本行档位」那一笔标脏在这里不发生 —— 脏由源提交后那条订阅打（默认口径两档都打，见 `on_signal_dirty`）。
    // 与「值写入」那条路可分辨：后者走 `assign` → 重订阅 + 本行档位，见上面的绘制档差额用例。
    AURORA_TEST_CHECK(dirty.paints > paints0);
    AURORA_TEST_CHECK(dirty.layouts > layouts0);

    const auto [count_after, ids_after] = collect_ids(b);
    AURORA_TEST_REQUIRE_EQ(count_after, count_before);  // 来源数不变
    AURORA_TEST_CHECK_TRUE(ids_after == ids_before);  // 且逐枚身份不变＝没有断绑换源

    const int paints_before_push = dirty.paints;
    const int layouts_before_push = dirty.layouts;
    owner.set(Color{0x01, 0x02, 0x03, 0xFF});  // 原订阅仍固定该来源，照常驱动控件
    AURORA_TEST_CHECK(b.color(BuildContext{}) == Color{0x01, 0x02, 0x03, 0xFF});
    AURORA_TEST_CHECK(dirty.paints > paints_before_push);  // 订阅回调落账
    AURORA_TEST_CHECK(dirty.layouts > layouts_before_push);  // 默认口径两档都打（`on_signal_dirty`）
}

AURORA_TEST_CASE(button_full_key_json_restores_every_effective_value) {
    // 逐行表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白属性对象，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    PropsProbe src;
    src.set_label("Legacy")
        .set_color(Color{0x11, 0x22, 0x33, 0xFF})
        .set_on_color(Color{0x44, 0x55, 0x66, 0xFF})
        .set_font_size(18.0F)
        .set_corner_radius(3.0F)
        .set_padding(EdgeInsets{.left = 5.0F, .top = 4.0F, .right = 5.0F, .bottom = 4.0F})
        .set_enabled(false)
        .set_hover_color(Color{0x77, 0x88, 0x99, 0xFF})
        .set_pressed_color(Color{0xAA, 0xBB, 0xCC, 0xFF})
        .set_border_color(Color{0xDD, 0xEE, 0xFF, 0xFF})
        .set_border_width(1.5F)
        .set_disabled_color(Color{0x12, 0x34, 0x56, 0xFF})
        .set_disabled_text_color(Color{0x78, 0x9A, 0xBC, 0xFF})
        .set_min_width(90.0F)
        .set_min_height(40.0F);

    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : {"label", "color", "on_color", "font_size", "corner_radius", "padding", "enabled",
                            "hover_color", "pressed_color", "border_color", "border_width", "disabled_color",
                            "disabled_text_color", "min_width", "min_height"}) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));  // 全量键：15 枚属性逐键在盘
    }

    PropsProbe dst;
    dst.deserialize_props(legacy);
    // 表态位逐行复原（缺一位就分不清「用户显式」与「系统默认」）。
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::on_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::font_size_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::padding_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::enabled_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::hover_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::pressed_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::border_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::border_width_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::disabled_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::disabled_text_color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::min_width_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ButtonProps::min_height_row()));

    // 逐属性生效值相等（两半读件都对照，含带上下文那三枚）。
    const BuildContext ctx;
    AURORA_TEST_CHECK_EQ(dst.label().text, src.label().text);
    AURORA_TEST_CHECK(dst.color(ctx) == src.color(ctx));
    AURORA_TEST_CHECK(dst.on_color(ctx) == src.on_color(ctx));
    AURORA_TEST_CHECK_NEAR(dst.font_size(), src.font_size(), 1e-6F);
    AURORA_TEST_CHECK_NEAR(dst.corner_radius(ctx), src.corner_radius(ctx), 1e-6F);
    const EdgeInsets pad = dst.padding(ctx);
    const EdgeInsets src_pad = src.padding(ctx);
    AURORA_TEST_CHECK_NEAR(pad.left, src_pad.left, 1e-6F);
    AURORA_TEST_CHECK_NEAR(pad.top, src_pad.top, 1e-6F);
    AURORA_TEST_CHECK_EQ(dst.enabled(), src.enabled());
    AURORA_TEST_CHECK(dst.hover_color(ctx) == src.hover_color(ctx));
    AURORA_TEST_CHECK(dst.pressed_color(ctx) == src.pressed_color(ctx));
    AURORA_TEST_CHECK(dst.border_color() == src.border_color());
    AURORA_TEST_CHECK_NEAR(dst.border_width(), src.border_width(), 1e-6F);
    AURORA_TEST_CHECK(dst.disabled_color() == src.disabled_color());
    AURORA_TEST_CHECK(dst.disabled_text_color() == src.disabled_text_color());
    AURORA_TEST_CHECK_NEAR(dst.min_width(), src.min_width(), 1e-6F);
    AURORA_TEST_CHECK_NEAR(dst.min_height(), src.min_height(), 1e-6F);

    // 快照佐证：同一份意图经「链式构造」与「旧全量键 JSON」两条路走来，必须落同一帧像素。
    LayoutEngine::layout(src, bounded(300.0F, 80.0F), ctx);
    LayoutEngine::layout(dst, bounded(300.0F, 80.0F), ctx);
    AURORA_TEST_CHECK_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(src.size().height, dst.size().height, 1e-4F);
    const Rect box{.origin = Point{}, .size = src.size()};
    const auto px_src = render_to_pixels(src, box, ctx, 300, 80);
    const auto px_dst = render_to_pixels(dst, box, ctx, 300, 80);
    AURORA_TEST_REQUIRE_EQ(px_src.size(), px_dst.size());
    AURORA_TEST_CHECK_TRUE(px_src == px_dst);
}

AURORA_TEST_CASE(button_invalid_json_degrades_to_row_fallback_without_stating) {
    // 控件侧的非法值口径（机制层 `utest_props` 同名校验的 Button 版本）：非法值一律不写入，
    // 可空行撤表态退回**该行回落点**，必填行保留现值，且全程经 `Diagnostics` 上报。
    // 本条锁的是迁移后的回归点：降级不能把当时的基线烘成用户意图。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    // ① 活令牌行越界（`corner_radius` 下界 0）⇒ 撤表态，生效值继续随主题档而变。
    Theme tuned = Theme::light();
    tuned.shapes.small = 13.0F;
    const auto [env_l, light] = themed_ctx(tuned);
    PropsProbe b;
    b.set_corner_radius(3.0F);
    Json below_min = Json::object();
    below_min.set("corner_radius", Json{-5.0});
    b.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(b.present(ButtonProps::corner_radius_row()));  // 撤的是表态位
    AURORA_TEST_CHECK_NEAR(b.corner_radius(light), 13.0F, 1e-4F);  // 退回回落点而非烘进 3.0 / -5.0
    Theme again = Theme::light();
    again.shapes.small = 6.0F;
    const auto [env_d, other] = themed_ctx(again);
    AURORA_TEST_CHECK_NEAR(b.corner_radius(other), 6.0F, 1e-4F);  // 降级后仍随档 ⇒ 没被钉成常量
    Json after_bad = Json::object();
    b.serialize_props(after_bad);
    AURORA_TEST_CHECK_FALSE(after_bad.contains("corner_radius"));  // 未表态 ⇒ 该键退出意图集合

    // ② 常量行类型不符（`font_size` 应为 number，喂串）⇒ 退回该行基线 14。
    PropsProbe c;
    c.set_font_size(18.0F);
    Json wrong_type = Json::object();
    wrong_type.set("font_size", Json{"not-a-number"});
    c.deserialize_props(wrong_type);
    AURORA_TEST_CHECK_FALSE(c.present(ButtonProps::font_size_row()));
    AURORA_TEST_CHECK_NEAR(c.font_size(), 14.0F, 1e-4F);  // 基线来自回落点，不是零值兜底
    AURORA_TEST_CHECK_NEAR(c.resolve(ButtonProps::font_size_row(), light), 14.0F, 1e-4F);

    // ③ 必填行（无表态位）吃到非法值 ⇒ 保留现值，绝不清空。
    PropsProbe d;
    d.set_label("Keep me");
    Json bad_label = Json::object();
    bad_label.set("label", Json{7.0});  // 既非纯串也非 `$msg` 对象
    d.deserialize_props(bad_label);
    AURORA_TEST_CHECK_EQ(d.label().text, std::string{"Keep me"});

    AURORA_TEST_CHECK(Diagnostics::count() >= 3);  // 三次降级逐一上报（不是静默吞掉）

    // ④ 反面对照（防空转）：同一路径灌合法值仍照常写穿并表态。
    Json good = Json::object();
    good.set("corner_radius", Json{9.0});
    good.set("font_size", Json{20.0});
    good.set("label", Json{"Fine"});
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(ButtonProps::corner_radius_row()));
    AURORA_TEST_CHECK_NEAR(d.corner_radius(light), 9.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.font_size(), 20.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(d.label().text, std::string{"Fine"});

    Diagnostics::take();
}

}  // namespace aurora::test_cases::utest_button
