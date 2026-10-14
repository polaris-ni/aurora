/// 测试类型: unit
/// 目标单元: include/aurora/widget/chip.h
/// 测试说明: 覆盖 Chip——标签/头像/颜色/字号链式 setter 与序列化、删除回调的命中区域语义
/// （右半区左键按下触发、其余忽略）、布局尺寸由文本度量构成；附带同头 Badge 的计数、
/// 尺寸加成、序列化与 describe

#include <memory>
#include <string>
#include <vector>

#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/font_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/chip.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_chip {

using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto press_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

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

}  // namespace

AURORA_TEST_CASE(default_and_labeled_construction) {
    const Chip empty;
    AURORA_TEST_CHECK_EQ(std::string{empty.type_name()}, "Chip");
    AURORA_TEST_CHECK_EQ(empty.label(), "");
    AURORA_TEST_CHECK_EQ(empty.avatar(), "");

    const Chip tagged("Tag");
    AURORA_TEST_CHECK_EQ(tagged.label(), "Tag");
    Json props = Json::object();
    tagged.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "label"), "Tag");
    AURORA_TEST_CHECK_FALSE(props.contains("avatar"));  // 空头像不落盘
    // CHG-011 迁移批：未显式设色不落盘，绘制时跟随主题 surface_variant（不再钉 230）。
    AURORA_TEST_CHECK_FALSE(props.contains("background"));
    AURORA_TEST_CHECK_FALSE(props.contains("text_color"));
    AURORA_TEST_CHECK_FALSE(props.contains("delete_color"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "font_size"), 13.0F, 1e-4F);
}

AURORA_TEST_CASE(chain_setters_update_serialized_props) {
    Chip c("base");
    c.set_label("renamed")
        .set_avatar("★")
        .set_background(Color(1, 2, 3, 4))
        .set_text_color(Color(5, 6, 7, 8))
        .set_delete_color(Color(9, 10, 11, 12))
        .set_font_size(15.0F)
        .set_corner_radius(6.0F);

    Json props = Json::object();
    c.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "label"), "renamed");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "avatar"), "★");
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "background"), 3)->as<int>().value(), 4);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "text_color"), 0)->as<int>().value(), 5);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "delete_color"), 1)->as<int>().value(), 10);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "font_size"), 15.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "corner_radius"), 6.0F, 1e-4F);
}

AURORA_TEST_CASE(font_size_non_positive_degrades_to_13) {
    Chip c;
    c.set_font_size(0.0F);  // 非正字号回落默认 13pt

    Json props = Json::object();
    c.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "font_size"), 13.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_sizes_from_text_metrics) {
    const Font f{.size_pt = 13.0F};  // 与 Chip 默认字号一致

    Chip plain("hi");
    LayoutEngine::layout(plain, bounded(1000.0F, 200.0F));
    const float expect_w = 20.0F + render::FontEngine::measure_width("hi", f);  // 10px 左右内边距
    AURORA_TEST_CHECK_NEAR(plain.size().width, expect_w, 1e-4F);
    AURORA_TEST_CHECK_NEAR(plain.size().height, render::FontEngine::measure_height(f) + 8.0F, 1e-4F);

    Chip with_avatar("hi");
    with_avatar.set_avatar("A");
    LayoutEngine::layout(with_avatar, bounded(1000.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(with_avatar.size().width, expect_w + render::FontEngine::measure_width("A", f) + 4.0F,
                           1e-4F);

    Chip deletable("hi");
    deletable.set_on_delete([]() -> void {});
    LayoutEngine::layout(deletable, bounded(1000.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(deletable.size().width, expect_w + render::FontEngine::measure_width(" ×", f), 1e-4F);
}

AURORA_TEST_CASE(delete_callback_fires_on_right_region_press) {
    Chip c("Tag");
    int fired = 0;
    c.set_on_delete([&fired]() -> void { ++fired; });
    LayoutEngine::layout(c, bounded(1000.0F, 200.0F));

    // 简化命中模型：右半区（x > 0.7 * 宽）左键按下即删除。
    MouseEvent hit = press_at(c.size().width * 0.85F, c.size().height * 0.5F);
    c.on_pointer_event(hit);
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_TRUE(hit.is_handled);

    MouseEvent miss = press_at(c.size().width * 0.2F, c.size().height * 0.5F);
    c.on_pointer_event(miss);
    AURORA_TEST_CHECK_EQ(fired, 1);  // 左半区不触发
    AURORA_TEST_CHECK_FALSE(miss.is_handled);
}

AURORA_TEST_CASE(non_press_or_non_left_clicks_ignored) {
    Chip c("Tag");
    int fired = 0;
    c.set_on_delete([&fired]() -> void { ++fired; });
    LayoutEngine::layout(c, bounded(1000.0F, 200.0F));

    MouseEvent move = press_at(c.size().width * 0.9F, c.size().height * 0.5F);
    move.action = MouseAction::Move;
    c.on_pointer_event(move);
    AURORA_TEST_CHECK_EQ(fired, 0);  // 非按下不触发

    MouseEvent right = press_at(c.size().width * 0.9F, c.size().height * 0.5F);
    right.button = MouseButton::Right;
    c.on_pointer_event(right);
    AURORA_TEST_CHECK_EQ(fired, 0);  // 非左键不触发
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Chip src("seed");
    src.set_avatar("A")
        .set_background(Color(1, 2, 3, 4))
        .set_text_color(Color(5, 6, 7, 8))
        .set_delete_color(Color(9, 10, 11, 12))
        .set_font_size(14.0F)
        .set_corner_radius(3.0F);

    Json props = Json::object();
    src.serialize_props(props);

    Chip dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.label(), "seed");
    AURORA_TEST_CHECK_EQ(dst.avatar(), "A");

    Json out = Json::object();
    dst.serialize_props(out);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "background"), 2)->as<int>().value(), 3);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "text_color"), 1)->as<int>().value(), 6);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "delete_color"), 3)->as<int>().value(), 12);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "font_size"), 14.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "corner_radius"), 3.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Chip::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Chip");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_on_delete = false;
    for (const auto &e : d.events) {
        if (std::string{e} == "on_delete") {
            has_on_delete = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_on_delete);
}

AURORA_TEST_CASE(badge_layout_adds_half_height_when_counted) {
    auto child_of = []() -> Node {
        auto t = std::make_shared<Text>(".");
        t->width(aurora::Length::fixed(40.0F));
        t->height(aurora::Length::fixed(20.0F));
        return Node{t};
    };

    Badge counted(5, child_of());
    LayoutEngine::layout(counted, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(counted.size().width, 40.0F, 1e-4F);  // max(子宽, 徽章宽 20)
    AURORA_TEST_CHECK_NEAR(counted.size().height, 29.0F, 1e-4F);  // 子高 20 + 徽章半高 9

    Badge zero(0, child_of());
    LayoutEngine::layout(zero, bounded(300.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(zero.size().height, 20.0F, 1e-4F);  // count=0 无高度加成
}

AURORA_TEST_CASE(badge_setters_serialize_and_describe) {
    Badge b(3);
    b.set_count(7).set_badge_color(Color(1, 2, 3, 4)).set_text_color(Color(5, 6, 7, 8));
    AURORA_TEST_CHECK_EQ(b.count(), 7);
    AURORA_TEST_CHECK_EQ(std::string{b.type_name()}, "Badge");

    Json props = Json::object();
    b.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "count"), 7);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "badge_color"), 0)->as<int>().value(), 1);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "text_color"), 1)->as<int>().value(), 6);

    Badge dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.count(), 7);

    const auto d = Badge::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Badge");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
}

// ---- 主题跟随（CHG-011 迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

AURORA_TEST_CASE(chip_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    const Chip c("Tag");
    // light 档：背景=surface_variant{226,226,226}、文字=on_surface 黑、删除=on_surface_variant{71,71,71}
    AURORA_TEST_CHECK(c.resolve_background(bare) == Color{0xE2, 0xE2, 0xE2});
    AURORA_TEST_CHECK(c.resolve_text_color(bare) == Color{0x00, 0x00, 0x00});
    AURORA_TEST_CHECK(c.resolve_delete_color(bare) == Color{0x47, 0x47, 0x47});

    // dark 档：背景=surface_variant{71,71,71}、文字=on_surface 白、删除=on_surface_variant{198,198,198}
    const auto [env, dark] = themed_ctx(Theme::dark());
    (void)env;
    AURORA_TEST_CHECK(c.resolve_background(dark) == Color{0x47, 0x47, 0x47});
    AURORA_TEST_CHECK(c.resolve_text_color(dark) == Color{0xFF, 0xFF, 0xFF});
    AURORA_TEST_CHECK(c.resolve_delete_color(dark) == Color{0xC6, 0xC6, 0xC6});
}

AURORA_TEST_CASE(chip_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    (void)env;
    Chip c("Tag");
    c.set_background(Color{0xE0, 0x1B, 0x24});  // error 红，刻意区别于两档 surface_variant
    c.set_text_color(Color{0x12, 0x34, 0x56});
    c.set_delete_color(Color{0xAA, 0xBB, 0xCC});
    AURORA_TEST_CHECK(c.resolve_background(dark) == Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK(c.resolve_text_color(dark) == Color{0x12, 0x34, 0x56});
    AURORA_TEST_CHECK(c.resolve_delete_color(dark) == Color{0xAA, 0xBB, 0xCC});
}

AURORA_TEST_CASE(badge_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light
    const Badge b(3);
    // light 档：徽章底=error{224,27,36}、文字=on_error 白（明暗 error/on_error 同值）
    AURORA_TEST_CHECK(b.resolve_badge_color(bare) == Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK(b.resolve_text_color(bare) == Color{0xFF, 0xFF, 0xFF});

    const auto [env, dark] = themed_ctx(Theme::dark());
    (void)env;
    AURORA_TEST_CHECK(b.resolve_badge_color(dark) == Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK(b.resolve_text_color(dark) == Color{0xFF, 0xFF, 0xFF});
}

AURORA_TEST_CASE(badge_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    (void)env;
    Badge b(3);
    b.set_badge_color(Color{0x12, 0x34, 0x56});
    b.set_text_color(Color{0xAA, 0xBB, 0xCC});
    AURORA_TEST_CHECK(b.resolve_badge_color(dark) == Color{0x12, 0x34, 0x56});
    AURORA_TEST_CHECK(b.resolve_text_color(dark) == Color{0xAA, 0xBB, 0xCC});
}

}  // namespace aurora::test_cases::utest_chip
