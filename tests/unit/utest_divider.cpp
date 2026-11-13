/// 测试类型: unit
/// 目标单元: include/aurora/widget/divider.h
/// 测试说明: 覆盖 Divider——默认属性与表态位、水平/垂直方向的布局尺寸（填满主轴、厚度作交叉轴）、
/// 厚度与紧约束钳制、链式写件与序列化往返、describe 元数据、主题跟随（未表态时取主题角色与
/// `controls.separator_thickness`，显式表态优先）。属性单一声明迁移后另覆盖权威表的读消费面：
/// 逐行撤表态该键退出序列化、常量档读件在两档主题下与库内解析全同、全量表态的 JSON 逐行复原
/// 生效值并落同一帧像素、非法 JSON 逐档降级（可空行撤表态退回回落点、降级经 Diagnostics 上报、
/// 未登记校验的枚举名按宽容通路退默认档且仍表态不上报）、挂载后按行档位标脏并真的取到新像素

#include <memory>
#include <string>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
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

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class DividerProbe final : public Divider {
  public:
    using Divider::present;
    using Divider::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct DividerDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(Divider &target) -> void {
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

}  // namespace

AURORA_TEST_CASE(default_state_and_type_name) {
    const Divider d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "Divider");

    Json props = Json::object();
    d.serialize_props(props);
    // 五行皆未表态 ⇒ 一律不写键（回落语义不外泄）；控件级 width/height/show 由基类写。
    AURORA_TEST_CHECK_FALSE(props.contains("orientation"));
    AURORA_TEST_CHECK_FALSE(props.contains("thickness"));
    AURORA_TEST_CHECK_FALSE(props.contains("color"));
    AURORA_TEST_CHECK_FALSE(props.contains("indent"));
    AURORA_TEST_CHECK_FALSE(props.contains("end_indent"));
    // 生效值取各行回落点：横向、主题线厚、主题线色、零缩进。
    const BuildContext bare;
    AURORA_TEST_CHECK(d.orientation() == Orientation::Horizontal);
    AURORA_TEST_CHECK_NEAR(d.thickness(bare), 1.0F, 1e-4F);
    AURORA_TEST_CHECK(d.color(bare) == Color(0xC6, 0xC6, 0xC6));
    AURORA_TEST_CHECK_NEAR(d.indent(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.end_indent(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(horizontal_fills_width_uses_thickness_height) {
    Divider d;
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 300.0F, 1e-4F);  // 横向填满父宽
    AURORA_TEST_CHECK_NEAR(d.size().height, 1.0F, 1e-4F);  // 高度 = 厚度
}

AURORA_TEST_CASE(vertical_fills_height_uses_thickness_width) {
    Divider d{DividerProps().set_orientation(Orientation::Vertical)};
    LayoutEngine::layout(d, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(d.size().width, 1.0F, 1e-4F);  // 宽度 = 厚度
    AURORA_TEST_CHECK_NEAR(d.size().height, 80.0F, 1e-4F);  // 纵向填满父高
}

AURORA_TEST_CASE(thickness_drives_cross_axis_size) {
    Divider h{DividerProps().set_thickness(4.0F)};
    LayoutEngine::layout(h, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(h.size().height, 4.0F, 1e-4F);

    Divider v{DividerProps().set_orientation(Orientation::Vertical).set_thickness(3.0F)};
    LayoutEngine::layout(v, bounded(300.0F, 80.0F));
    AURORA_TEST_CHECK_NEAR(v.size().width, 3.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_clamps_into_tight_constraints) {
    Divider d{DividerProps().set_thickness(6.0F)};
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
    Divider src{DividerProps()
                    .set_orientation(Orientation::Vertical)
                    .set_thickness(2.5F)
                    .set_color(Color(10, 20, 30, 40))
                    .set_indent(6.0F)
                    .set_end_indent(3.0F)};
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
    AURORA_TEST_CHECK(d.color(bare) == Color(0xC6, 0xC6, 0xC6));  // light outline_variant

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(d.color(dark) == Color(0x47, 0x47, 0x47));  // dark outline_variant
}

AURORA_TEST_CASE(divider_explicit_color_overrides_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    Divider d;
    d.set_color(Color{10, 20, 30, 40});  // 刻意区别于两档 outline_variant
    AURORA_TEST_CHECK(d.color(dark) == Color(10, 20, 30, 40));
}

// ---- 度量跟随（接线批）：未显式设厚度 → 读主题 `controls.separator_thickness` ----

AURORA_TEST_CASE(divider_thickness_default_to_material_baseline) {
    // 无注入 = Material 基线档：接线前后实测同为 1dp，观感零回归的锚点。
    const BuildContext bare;
    const Divider d;
    AURORA_TEST_CHECK_NEAR(d.thickness(bare), 1.0F, 1e-4F);
}

AURORA_TEST_CASE(divider_thickness_follows_theme_tokens_when_unset) {
    // 改过令牌的主题 ⇒ 解析值随令牌走（证明读的是 theme，而不是写死的常量）。
    Theme t = Theme::light();
    t.controls.separator_thickness = 3.5F;
    const auto [env, ctx] = themed_ctx(t);
    Divider d;
    AURORA_TEST_CHECK_NEAR(d.thickness(ctx), 3.5F, 1e-4F);
    LayoutEngine::layout(d, bounded(300.0F, 80.0F), ctx);
    AURORA_TEST_CHECK_NEAR(d.size().height, 3.5F, 1e-4F);  // 布局与绘制同源（同一条 resolve 通道）
}

AURORA_TEST_CASE(divider_explicit_thickness_overrides_theme) {
    // 对照组：显式设过厚度后主题不动（两态语义的另一半）。
    Theme t = Theme::light();
    t.controls.separator_thickness = 3.5F;
    const auto [env, ctx] = themed_ctx(t);
    Divider d;
    d.set_thickness(6.0F);
    AURORA_TEST_CHECK_NEAR(d.thickness(ctx), 6.0F, 1e-4F);
    // 撤表态又回到档：公共面上唯一的 `clear(row)` 是撤销表态的唯一通道。
    d.clear(DividerProps::thickness_row());
    AURORA_TEST_CHECK_NEAR(d.thickness(ctx), 3.5F, 1e-4F);
}

// ===========================================================================
// 权威表的读消费面（属性单一声明迁移批）：逐行撤表态 / 常量档两档恒等 / 全量键复原 /
// 非法值降级 / 挂载后逐档标脏并取到新像素
// ===========================================================================

AURORA_TEST_CASE(divider_clear_unstates_each_row_and_drops_every_key) {
    // 五行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且取值回到该行回落点」。
    // 常量档回到自己的基线，活令牌档回到随作用域而变的档——撤的是表态位，不是把当时的值烘进字段。
    Theme tuned = Theme::light();
    tuned.controls.separator_thickness = 3.5F;
    const auto [env, ctx] = themed_ctx(tuned);

    DividerProbe d;
    d.set_orientation(Orientation::Vertical)
        .set_thickness(2.0F)
        .set_color(Color{0x11, 0x22, 0x33, 0x44})
        .set_indent(6.0F)
        .set_end_indent(4.0F);
    Json stated = Json::object();
    d.serialize_props(stated);
    for (const char *key : {"orientation", "thickness", "color", "indent", "end_indent"}) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 五行全表态 ⇒ 五键逐键在盘
    }

    d.clear(DividerProps::orientation_row());
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::orientation_row()));
    AURORA_TEST_CHECK(d.orientation() == Orientation::Horizontal);  // 常量档回落基线

    d.clear(DividerProps::color_row());
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::color_row()));
    AURORA_TEST_CHECK(d.color(ctx) == Theme::light().roles.outline_variant);  // 活令牌档回到当档

    d.clear(DividerProps::indent_row());
    d.clear(DividerProps::end_indent_row());
    AURORA_TEST_CHECK_NEAR(d.indent(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.end_indent(), 0.0F, 1e-4F);

    d.clear(DividerProps::thickness_row());
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::thickness_row()));
    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const char *key : {"orientation", "thickness", "color", "indent", "end_indent"}) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }

    // 换档再度生效 ⇒ 降级后仍随档，没被钉成常量。
    Theme again = Theme::light();
    again.controls.separator_thickness = 9.0F;
    const auto [env2, ctx2] = themed_ctx(again);
    AURORA_TEST_CHECK_NEAR(d.thickness(ctx2), 9.0F, 1e-4F);
    AURORA_TEST_CHECK(d.color(ctx2) == Theme::light().roles.outline_variant);
}

AURORA_TEST_CASE(divider_constant_rows_read_identically_under_light_and_dark) {
    // 恒等关系的正面取证：常量档行没有 `x(ctx)` 第二件，其无参读件**就是**生效值——
    // 明 / 暗两档主题下与库内 `resolve(row, ctx)` 取值全同，缺这组断言则「窄面不丢语义」只是断言。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    DividerProbe d;

    AURORA_TEST_CHECK(d.orientation() == d.resolve(DividerProps::orientation_row(), light));
    AURORA_TEST_CHECK(d.orientation() == d.resolve(DividerProps::orientation_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.indent(), d.resolve(DividerProps::indent_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.indent(), d.resolve(DividerProps::indent_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.end_indent(), d.resolve(DividerProps::end_indent_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.end_indent(), d.resolve(DividerProps::end_indent_row(), dark), 1e-6F);
    // 表态后仍与解析同值（两半都恒等，才排除「无参读件读的是另一份存储」）。
    d.set_indent(5.0F).set_orientation(Orientation::Vertical);
    AURORA_TEST_CHECK_NEAR(d.indent(), d.resolve(DividerProps::indent_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.indent(), 5.0F, 1e-6F);
    AURORA_TEST_CHECK(d.orientation() == Orientation::Vertical);
}

AURORA_TEST_CASE(divider_full_key_json_restores_every_effective_value) {
    // 逐行表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态（旧写件恒写
    // orientation / indent / end_indent，另两键只在设值时写）；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    Divider src{DividerProps()
                    .set_orientation(Orientation::Vertical)
                    .set_thickness(2.5F)
                    .set_color(Color{0x12, 0x34, 0x56, 0x78})
                    .set_indent(3.0F)
                    .set_end_indent(1.0F)};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : {"orientation", "thickness", "color", "indent", "end_indent"}) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }

    DividerProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(DividerProps::orientation_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(DividerProps::thickness_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(DividerProps::color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(DividerProps::indent_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(DividerProps::end_indent_row()));
    AURORA_TEST_CHECK(dst.orientation() == Orientation::Vertical);
    AURORA_TEST_CHECK_NEAR(dst.thickness(ctx), 2.5F, 1e-4F);
    AURORA_TEST_CHECK(dst.color(ctx) == Color{0x12, 0x34, 0x56, 0x78});
    AURORA_TEST_CHECK_NEAR(dst.indent(), 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.end_indent(), 1.0F, 1e-4F);

    // 同一帧像素：复原出来的控件与逐行表态构造出来的控件画得逐位相同。
    LayoutEngine::layout(src, bounded(60.0F, 40.0F), ctx);
    LayoutEngine::layout(dst, bounded(60.0F, 40.0F), ctx);
    AURORA_TEST_REQUIRE_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_REQUIRE_NEAR(src.size().height, dst.size().height, 1e-4F);
    const Rect box{.origin = Point{}, .size = src.size()};
    const auto px_src = render_to_pixels(src, box, ctx, 60, 40);
    const auto px_dst = render_to_pixels(dst, box, ctx, 60, 40);
    AURORA_TEST_REQUIRE_EQ(px_src.size(), px_dst.size());
    AURORA_TEST_CHECK_TRUE(px_src == px_dst);
}

AURORA_TEST_CASE(divider_invalid_json_degrades_to_row_fallback_without_stating) {
    // 控件侧的非法值口径（机制层校验的 Divider 版本）：非法值一律不写入，可空行撤表态退回
    // **该行回落点**，且降级经 `Diagnostics` 上报。本条锁的是迁移后的回归点：
    // 降级不能把当时的基线烘成用户意图。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    // ① 活令牌行越界（`thickness` 下界 0）⇒ 撤表态，生效值继续随主题档而变。
    Theme tuned = Theme::light();
    tuned.controls.separator_thickness = 3.5F;
    const auto [env_l, light] = themed_ctx(tuned);
    DividerProbe d;
    d.set_thickness(2.0F);
    Json below_min = Json::object();
    below_min.set("thickness", Json{-5.0});
    d.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::thickness_row()));  // 撤的是表态位
    AURORA_TEST_CHECK_NEAR(d.thickness(light), 3.5F, 1e-4F);  // 退回回落点而非烘进 2.0 / -5.0
    Theme again = Theme::light();
    again.controls.separator_thickness = 6.0F;
    const auto [env_d, other] = themed_ctx(again);
    AURORA_TEST_CHECK_NEAR(d.thickness(other), 6.0F, 1e-4F);  // 降级后仍随档 ⇒ 没被钉成常量

    // ② 常量行类型不符（`indent` 应为 number，喂串）⇒ 退回该行基线 0。
    d.set_indent(7.0F);
    Json wrong_type = Json::object();
    wrong_type.set("indent", Json{"not-a-number"});
    d.deserialize_props(wrong_type);
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::indent_row()));
    AURORA_TEST_CHECK_NEAR(d.indent(), 0.0F, 1e-4F);  // 基线来自回落点，不是零值兜底之外的另一份真值
    AURORA_TEST_CHECK_NEAR(d.resolve(DividerProps::indent_row(), light), 0.0F, 1e-4F);

    // ③ 颜色形态不符（应为四元数组，喂串）⇒ 同样撤表态退回 `roles.outline_variant`。
    d.set_color(Color{0xAA, 0xBB, 0xCC, 0xFF});
    Json bad_color = Json::object();
    bad_color.set("color", Json{"red"});
    d.deserialize_props(bad_color);
    AURORA_TEST_CHECK_FALSE(d.present(DividerProps::color_row()));
    AURORA_TEST_CHECK(d.color(light) == tuned.roles.outline_variant);

    AURORA_TEST_CHECK(Diagnostics::count() >= 3);  // 三次降级逐一上报（不是静默吞掉）

    // ④ 枚举名走的是另一条通路（本批如实登记的口径边界）：`PropCodec<Orientation>` 未登记 `validate`，
    //     未知名字按 `json_to_orientation` 的既定行为退默认档**且照常写入表态位**，不产生降级上报。
    //     「非法值一律不写入」只约束已登记判据的类型（数值越界 / 形态不符），枚举名不在其列。
    const std::size_t reported = Diagnostics::count();
    d.clear(DividerProps::orientation_row());
    Json unknown_enum = Json::object();
    unknown_enum.set("orientation", Json{"diagonal"});
    d.deserialize_props(unknown_enum);
    AURORA_TEST_CHECK_TRUE(d.present(DividerProps::orientation_row()));  // 宽容通路照样表态
    AURORA_TEST_CHECK(d.orientation() == Orientation::Horizontal);  // 未知名 ⇒ 该枚举的默认档
    AURORA_TEST_CHECK(Diagnostics::count() == reported);  // 不报降级（与迁移前的宽容解析逐字一致）

    // ⑤ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("thickness", Json{4.0});
    good.set("indent", Json{2.0});
    good.set("orientation", Json{"vertical"});
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(DividerProps::thickness_row()));
    AURORA_TEST_CHECK_NEAR(d.thickness(light), 4.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.indent(), 2.0F, 1e-4F);
    AURORA_TEST_CHECK(d.orientation() == Orientation::Vertical);

    Diagnostics::take();
}

AURORA_TEST_CASE(divider_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入必须按行上声明的档位标脏：`.layout().paint()` 两档各落一笔，`.paint()` 只落绘制档。
    Divider d;
    const BuildContext ctx;
    d.mount(ctx);
    DividerDirty dirty;
    dirty.attach_to(d);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    d.set_thickness(3.0F);  // `.layout().paint()`：两档各一次
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    d.set_indent(4.0F);  // 纯绘制档行：只落绘制档，不重排
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before + 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before);

    const Color next{0x12, 0x34, 0x56, 0xFF};
    d.set_color(next);  // 同值守卫的另一半：同值再写不落账
    const int paints_after_color = dirty.paints;
    d.set_color(next);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_color);

    // 「取到新画面」：落笔的线像素真的换了色，不是只记了一笔脏；且起点缩进真的把线头推后了。
    LayoutEngine::layout(d, bounded(40.0F, 20.0F), ctx);
    const auto px = render_to_pixels(d, Rect{.origin = Point{}, .size = d.size()}, ctx, 40, 20);
    AURORA_TEST_REQUIRE_FALSE(px.empty());
    AURORA_TEST_CHECK_FALSE(px[0] == next);  // x=0 落在 indent=4 之前：线上无线
    AURORA_TEST_CHECK(px[4] == next);  // x=4 起是线身（行宽 40，y=0 故下标即 x）
    AURORA_TEST_CHECK_NEAR(d.size().height, 3.0F, 1e-4F);

    // 方向翻转走布局 + 绘制两档，且主轴语义随之改变。
    const int tier_paints = dirty.paints;
    const int tier_layouts = dirty.layouts;
    d.set_orientation(Orientation::Vertical);
    AURORA_TEST_CHECK_EQ(dirty.paints, tier_paints + 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts, tier_layouts + 1);
    LayoutEngine::layout(d, bounded(40.0F, 20.0F), ctx);
    AURORA_TEST_CHECK_NEAR(d.size().width, 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.size().height, 20.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_divider
