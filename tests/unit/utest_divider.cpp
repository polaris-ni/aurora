/// 测试类型: unit
/// 目标单元: include/aurora/widget/divider.h
/// 测试说明: 覆盖 Divider——默认属性、水平/垂直方向的布局尺寸（填满主轴、厚度作交叉轴）、
/// 厚度与紧约束钳制、缩进链式 setter、序列化往返与 describe 元数据

#include <memory>
#include <string>

#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/divider.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_divider {

using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
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

AURORA_TEST_CASE(default_state_and_type_name) {
    const Divider d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "Divider");

    Json props = Json::object();
    d.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "orientation"), "horizontal");
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "thickness"), 1.0F, 1e-4F);
    // 默认未显式设色：color 键不输出（遵循「跟随主题 outline_variant」语义，与 Button/Text 同口径）。
    AURORA_TEST_CHECK_FALSE(props.contains("color"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "indent"), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "end_indent"), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(horizontal_fills_width_uses_thickness_height) {
    Divider d;
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 300.0F, 1e-4F);  // 横向填满父宽
    AURORA_TEST_CHECK_NEAR(d.size().height, 1.0F, 1e-4F);  // 高度 = 厚度
}

AURORA_TEST_CASE(vertical_fills_height_uses_thickness_width) {
    Divider d{DividerProps{.orientation = Orientation::Vertical}};
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 1.0F, 1e-4F);  // 宽度 = 厚度
    AURORA_TEST_CHECK_NEAR(d.size().height, 80.0F, 1e-4F);  // 纵向填满父高
}

AURORA_TEST_CASE(thickness_drives_cross_axis_size) {
    Divider h{DividerProps{.thickness = 4.0F}};
    LayoutEngine::layout(h, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(h.size().height, 4.0F, 1e-4F);

    Divider v{DividerProps{.orientation = Orientation::Vertical, .thickness = 3.0F}};
    LayoutEngine::layout(v, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(v.size().width, 3.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Divider d{DividerProps{.thickness = 6.0F}};
    LayoutEngine::layout(d, bounded(20.0F, 2.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.size().height, 2.0F, 1e-4F);  // 请求厚度 6 被钳到 max 2
}

AURORA_TEST_CASE(indent_setters_chain_and_serialize) {
    Divider d;
    d.set_indent(8.0F).set_end_indent(4.0F);  // 链式

    Json props = Json::object();
    d.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "indent"), 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "end_indent"), 4.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_deserialize_roundtrip) {
    Divider src{DividerProps{.orientation = Orientation::Vertical,
                             .thickness = 2.5F,
                             .color = Color(10, 20, 30, 40),
                             .indent = 6.0F,
                             .end_indent = 3.0F}};
    Json props = Json::object();
    src.serialize_props(props);

    Divider dst;
    dst.deserialize_props(props);
    Json out = Json::object();
    dst.serialize_props(out);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(out, "orientation"), "vertical");
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "thickness"), 2.5F, 1e-4F);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "color"), 1)->as<int>().value(), 20);
    AURORA_TEST_CHECK_EQ(require_child_at(*require_child(out, "color"), 3)->as<int>().value(), 40);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "indent"), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "end_indent"), 3.0F, 1e-4F);

    // 方向随 JSON 翻转：布局尺寸按垂直语义重算。
    LayoutEngine::layout(dst, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 2.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.size().height, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Divider::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Divider");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool has_orientation = false;
    bool has_thickness = false;
    bool has_indent = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "orientation") {
            has_orientation = true;
        }
        if (std::string{p.name} == "thickness") {
            has_thickness = true;
        }
        if (std::string{p.name} == "indent") {
            has_indent = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_orientation);
    AURORA_TEST_CHECK_TRUE(has_thickness);
    AURORA_TEST_CHECK_TRUE(has_indent);
}

// ---- 主题跟随（迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

AURORA_TEST_CASE(divider_color_follows_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题 outline_variant；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    const Divider d;
    AURORA_TEST_CHECK(d.resolve_color(bare) == Color(0xC6, 0xC6, 0xC6));  // light outline_variant

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(d.resolve_color(dark) == Color(0x47, 0x47, 0x47));  // dark outline_variant
}

AURORA_TEST_CASE(divider_explicit_color_overrides_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    Divider d;
    d.color = Color{10, 20, 30, 40};  // 刻意区别于两档 outline_variant
    AURORA_TEST_CHECK(d.resolve_color(dark) == Color(10, 20, 30, 40));
}

}  // namespace aurora::test_cases::utest_divider
