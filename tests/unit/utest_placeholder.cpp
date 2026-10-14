/// 测试类型: unit
/// 目标单元: include/aurora/widget/placeholder.h
/// 测试说明: 覆盖 Placeholder 降级占位控件——默认文案回退 "(placeholder)"、链式 setter、
/// 尺寸由文本测量 + 内边距构成、颜色序列化往返、自描述与信号缺席

#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/placeholder.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "framework/json_literals.h"

namespace aurora::test_cases::utest_placeholder {
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(default_follows_theme_roles_when_colors_unset) {
    const Placeholder p;
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "Placeholder");

    Json props = Json::object();
    p.serialize_props(props);
    // 空消息序列化原样空串（绘制期才回退 "(placeholder)"）。
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "message"), "");
    // 默认三色未显式设置：不输出键，跟随主题角色（CHG-011 迁移批）。
    AURORA_TEST_CHECK_FALSE(props.contains("background_color"));
    AURORA_TEST_CHECK_FALSE(props.contains("border_color"));
    AURORA_TEST_CHECK_FALSE(props.contains("text_color"));

    // 裸 ctx（无主题注入）→ 回落 light 角色。
    const BuildContext bare;
    AURORA_TEST_CHECK(p.resolve_background(bare) == Color{0xFF, 0xFF, 0xFF});  // light surface
    AURORA_TEST_CHECK(p.resolve_border(bare) == Color{0xE0, 0x1B, 0x24});  // light error
    AURORA_TEST_CHECK(p.resolve_text(bare) == Color{0x47, 0x47, 0x47});  // light on_surface_variant
}

AURORA_TEST_CASE(chain_setters_update_state) {
    Placeholder p;
    p.set_message("boom")
        .set_background_color(Color(1, 2, 3, 4))
        .set_border_color(Color(5, 6, 7, 8))
        .set_text_color(Color(9, 10, 11, 12));

    Json props = Json::object();
    p.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "message"), "boom");
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "background_color"), 2)->as_or<int>(0), 3);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "border_color"), 1)->as_or<int>(0), 6);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(props, "text_color"), 3)->as_or<int>(0), 12);
}

AURORA_TEST_CASE(layout_sizes_from_text_metrics_plus_padding) {
    Placeholder empty;
    LayoutEngine::layout(empty, bounded(500.0F, 200.0F));
    const Size a = empty.size();
    AURORA_TEST_CHECK_TRUE(a.width > 16.0F);  // 至少含 16px 水平内边距
    AURORA_TEST_CHECK_TRUE(a.height > 12.0F);

    // 空消息渲染默认文案 "(placeholder)"；显式短消息更窄，单行高度不变。
    Placeholder msg("x");
    LayoutEngine::layout(msg, bounded(500.0F, 200.0F));
    const Size b = msg.size();
    AURORA_TEST_CHECK_TRUE(b.width < a.width);
    AURORA_TEST_CHECK_NEAR(b.height, a.height, 1e-4F);  // 高度只依赖字号

    // 更长消息更宽或相等（不会更窄）。
    Placeholder longer("this is a fairly long placeholder message");
    LayoutEngine::layout(longer, bounded(5000.0F, 200.0F));
    const Size c = longer.size();
    AURORA_TEST_CHECK_TRUE(c.width >= a.width - 0.5F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Placeholder p("hello");
    LayoutEngine::layout(p, bounded(30.0F, 20.0F));
    const Size s = p.size();
    AURORA_TEST_CHECK_NEAR(s.width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(props_deserialize_roundtrip) {
    Json props = Json::object();
    props.set("message", "restored");
    props.set("background_color", testing::json_arr({10, 20, 30, 40}));
    props.set("border_color", testing::json_arr({50, 60, 70, 80}));
    props.set("text_color", testing::json_arr({90, 100, 110, 120}));

    Placeholder p;
    p.deserialize_props(props);
    Json out = Json::object();
    p.serialize_props(out);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(out, "message"), "restored");
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "background_color"), 0)->as_or<int>(0), 10);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "border_color"), 1)->as_or<int>(0), 60);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "text_color"), 2)->as_or<int>(0), 110);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Placeholder::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Placeholder");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_message = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "message") {
            has_message = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_message);

    // 无自有信号。
    Placeholder p;
    std::vector<aurora::SignalViewBase *> out;
    p.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.size(), 0U);
}

// ---- 主题跟随（CHG-011 迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

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

AURORA_TEST_CASE(placeholder_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    Placeholder p;
    AURORA_TEST_CHECK(p.resolve_background(bare) == Color{0xFF, 0xFF, 0xFF});  // light surface
    AURORA_TEST_CHECK(p.resolve_border(bare) == Color{0xE0, 0x1B, 0x24});  // light error
    AURORA_TEST_CHECK(p.resolve_text(bare) == Color{0x47, 0x47, 0x47});  // light on_surface_variant

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(p.resolve_background(dark) == Color{0x20, 0x21, 0x24});  // dark surface
    AURORA_TEST_CHECK(p.resolve_border(dark) == Color{0xE0, 0x1B, 0x24});  // dark error（明暗同值）
    AURORA_TEST_CHECK(p.resolve_text(dark) == Color{0xC6, 0xC6, 0xC6});  // dark on_surface_variant
}

AURORA_TEST_CASE(placeholder_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    Placeholder p;
    p.set_background_color(Color{0x12, 0x34, 0x56});
    p.set_border_color(Color{0xAB, 0xCD, 0xEF});
    p.set_text_color(Color{0x11, 0x22, 0x33});
    AURORA_TEST_CHECK(p.resolve_background(dark) == Color{0x12, 0x34, 0x56});
    AURORA_TEST_CHECK(p.resolve_border(dark) == Color{0xAB, 0xCD, 0xEF});
    AURORA_TEST_CHECK(p.resolve_text(dark) == Color{0x11, 0x22, 0x33});
}

}  // namespace aurora::test_cases::utest_placeholder
