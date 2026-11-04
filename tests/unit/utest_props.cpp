/// 测试类型: unit
/// 目标单元: include/aurora/widget/props.h
/// 测试说明: 覆盖属性机制层的派生契约。夹具 ProbeProps 一次覆盖四格字段形状（必填值件 / 必填来源件 /
/// 可空值件 / 可空来源件）与回落点三形态（常量档 / 活令牌档 / 派生档）。逐条判据：
///   1. 必填位与来源位由字段形状推出，裸值类型剥掉可空与句柄两层外壳；
///   2. `clear` 对必填行在重载集里不存在（编译期拒绝，不是运行期判空），对可空行退回该行回落点；
///   3. 常量档读面无需上下文；活令牌档随注入主题而变，表态后不再跟随；派生档未表态时读同族他行；
///   4. 序列化：必填行恒写、非必填行只在表态时写；反序列化：来源型写进**当前源**（不换绑，后续
///      `State::set` 仍驱动该属性），非法值降级为该行回落基线并经 Diagnostics 上报；
///   5. 描述符条目顺序 = `rows()` 点名顺序，约束旁注落到 `minimum` / `maximum` / `requires_props`，
///      默认值表示按档位分化（常量档给原生值，令牌档给带引号的令牌短名，派生档给显示串）；
///   6. `collect_prop_sources` 只登记来源型行，且未表态的可空来源不登记（没有源可订）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "aurora/core/color.h"
#include "aurora/core/diagnostics.h"
#include "aurora/environment/build_context.h"
#include "aurora/environment/environment.h"
#include "aurora/state/state.h"
#include "aurora/theming/color_roles.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/props.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_props {

namespace probe {

/// @brief 机制层夹具：形态与真实控件迁移后的形态一致（公有继承基类 + 一行一字段 + rows() 点名）。
class ProbeProps : public AbstractProps<ProbeProps> {
  public:
    // ---- 权威表：每行一个私有字段，键名与字段钉在同一实参位 ----

    /// @brief 必填来源件：裸句柄 ⇒ 不可撤表态，读面为无参 `label()`。
    [[nodiscard]] static constexpr auto label_row() {
        return prop_row<&ProbeProps::label_>("label").paint().required().note("Row label");
    }

    /// @brief 必填值件：裸 `float` ⇒ 回落点即字段自身，带上下界与依赖旁注。
    [[nodiscard]] static constexpr auto size_row() {
        return prop_row<&ProbeProps::size_>("size")
            .layout()
            .paint()
            .required()
            .bounds("0", "100")
            .requires_prop("label")
            .note("Box size");
    }

    /// @brief 活令牌档：回落点为 `ColorRoles::primary` ⇒ 本行发带上下文读件。
    [[nodiscard]] static constexpr auto color_row() {
        return prop_row<&ProbeProps::color_>("color").paint().fallback<&ColorRoles::primary>();
    }

    /// @brief 常量档：回落点为库内常量提供者 ⇒ 本行发无上下文读件。
    [[nodiscard]] static constexpr auto radius_row() {
        return prop_row<&ProbeProps::radius_>("radius").layout().fallback(&ProbeProps::radius_baseline);
    }

    /// @brief 派生档：回落点读同族他行的生效值 ⇒ 必须给显示串（无实例求不出值）。
    [[nodiscard]] static constexpr auto hover_row() {
        return prop_row<&ProbeProps::hover_>("hover_color")
            .paint()
            .fallback(&ProbeProps::hover_baseline)
            .shows("darkened color");
    }

    /// @brief 无相等比较的类型（EdgeInsets）：写面的判等短路必须由 `if constexpr` 摘掉。
    [[nodiscard]] static constexpr auto insets_row() {
        return prop_row<&ProbeProps::insets_>("padding").layout().fallback(&ProbeProps::insets_baseline);
    }

    /// @brief 可空来源件：`std::optional<ReadOnlyState<float>>` ⇒ 既可撤表态，也有「写当前源」通道。
    [[nodiscard]] static constexpr auto opacity_row() {
        return prop_row<&ProbeProps::opacity_>("opacity").paint().fallback(&ProbeProps::opacity_baseline);
    }

    /// @brief 权威表：参数顺序即 schema 顺序。
    [[nodiscard]] static constexpr auto rows() {
        return make_prop_rows(label_row(), size_row(), color_row(), radius_row(), hover_row(), insets_row(),
                              opacity_row());
    }

    // ---- 读面：每行恰一件，形态由该行回落点决定（来源行也只有句柄重载这一个同名写口的对应读件）----

    [[nodiscard]] auto label() const -> std::string { return get(label_row()); }
    [[nodiscard]] auto size() const -> float { return get(size_row()); }
    [[nodiscard]] auto radius() const -> float { return stated(radius_row()); }
    [[nodiscard]] auto color(const BuildContext &ctx) const -> Color { return resolve(color_row(), ctx); }
    [[nodiscard]] auto hover(const BuildContext &ctx) const -> Color { return resolve(hover_row(), ctx); }
    [[nodiscard]] auto insets() const -> EdgeInsets { return stated(insets_row()); }
    [[nodiscard]] auto opacity() const -> float { return stated(opacity_row()); }

    // ---- 写面 ----

    auto set_label(const std::string &value) -> ProbeProps & { return assign(label_row(), value); }
    auto set_size(float value) -> ProbeProps & { return assign(size_row(), value); }
    auto set_color(Color value) -> ProbeProps & { return assign(color_row(), value); }
    auto set_radius(float value) -> ProbeProps & { return assign(radius_row(), value); }
    auto set_hover(Color value) -> ProbeProps & { return assign(hover_row(), value); }
    auto set_insets(EdgeInsets value) -> ProbeProps & { return assign(insets_row(), value); }
    auto set_opacity(float value) -> ProbeProps & { return assign(opacity_row(), value); }
    auto bind_opacity(ReadOnlyState<float> source) -> ProbeProps & { return rebind(opacity_row(), std::move(source)); }
    auto bind_label(ReadOnlyState<std::string> source) -> ProbeProps & {
        return rebind(label_row(), std::move(source));
    }

    // ---- 库内消费面的夹具出口（真实控件里这些只由序列化与信号采集转调）----

    [[nodiscard]] auto is_radius_stated() const -> bool { return present(radius_row()); }
    [[nodiscard]] auto is_color_stated() const -> bool { return present(color_row()); }
    [[nodiscard]] auto is_opacity_stated() const -> bool { return present(opacity_row()); }
    [[nodiscard]] auto opacity_handle() const -> ReadOnlyState<float> { return read(opacity_row()); }
    [[nodiscard]] static auto describe() -> WidgetDescriptor { return props_descriptor("Probe"); }
    auto serialize(Json &out) const -> void { props_serialize(out); }
    auto deserialize(const Json &in) -> void { props_deserialize(in); }
    auto collect(StateSink &out) const -> void { collect_prop_sources(out); }

    /// @brief 常量档回落点：全库唯一基线。
    [[nodiscard]] static auto radius_baseline() -> float { return 4.0F; }

    /// @brief 派生档回落点：读同族他行（`color` 行）的生效值再压暗。
    [[nodiscard]] static auto hover_baseline(const ProbeProps &self, const BuildContext &ctx) -> Color {
        const Color base = self.resolve(color_row(), ctx);
        return Color{static_cast<std::uint8_t>(base.r / 2), static_cast<std::uint8_t>(base.g / 2),
                     static_cast<std::uint8_t>(base.b / 2), base.a};
    }

    /// @brief 无相等比较类型的回落点。
    [[nodiscard]] static auto insets_baseline() -> EdgeInsets {
        return EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F};
    }

    /// @brief 可空来源件的回落点。
    [[nodiscard]] static auto opacity_baseline() -> float { return 1.0F; }

  private:
    ReadOnlyState<std::string> label_;  ///< 必填来源件
    float size_ = 0.0F;  ///< 必填值件
    std::optional<Color> color_;  ///< 活令牌档
    std::optional<float> radius_;  ///< 常量档
    std::optional<Color> hover_;  ///< 派生档
    std::optional<EdgeInsets> insets_;  ///< 常量档（无 operator==）
    std::optional<ReadOnlyState<float>> opacity_;  ///< 可空来源件

    friend class AbstractProps<ProbeProps>;
};

/// @brief 「这行能不能撤表态」的可检测性：必填行必须在重载集里根本没有 `clear`。
template <typename Row>
concept clearable = requires(ProbeProps props, Row row) { props.clear(row); };

/// @brief 夹具控件：迁移后的真实控件只剩「公有继承 Props + 虚函数一行转调 + 构造尾部一次 `bind`」，
///        本夹具照同一形态写 —— 宿主接线位与移动面重绑的全部判据都落在这只控件上（见文末三例）。
class ProbeWidget : public LeafWidget, public ProbeProps {
  public:
    /// @brief 构造：按值接住属性对象（移动落位），尾部一次接线宿主 —— 控件侧唯一的接线仪式。
    /// @param props 已链式配好的属性对象（此刻还是脱离宿主的 DTO，其写入不落任何标脏）。
    explicit ProbeWidget(ProbeProps props = ProbeProps()) : ProbeProps(std::move(props)) { ProbeProps::bind(*this); }

    [[nodiscard]] auto type_name() const -> const char * override { return "ProbeWidget"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz_); }
    auto on_paint(Painter &p, const Rect &b, const BuildContext & /*ctx*/) -> void override { p.fill_rect(b, col_); }
    /// @brief 信号采集一行转调：来源型行的订阅面由表派生（`bind` 之后这条通道才活着）。
    auto collect_signals(StateSink &out) -> void override { collect_prop_sources(out); }

  private:
    Size sz_{.width = 10.0F, .height = 10.0F};  ///< 固定尺寸（本夹具不测布局）
    Color col_{0xFF, 0x00, 0x00, 0xFF};  ///< 固定底色（本夹具不测像素）
};

}  // namespace probe

using probe::ProbeProps;
using probe::ProbeWidget;

AURORA_TEST_CASE(field_shape_drives_required_and_source_bits) {
    using LabelRow = decltype(ProbeProps::label_row());
    using SizeRow = decltype(ProbeProps::size_row());
    using ColorRow = decltype(ProbeProps::color_row());
    using OpacityRow = decltype(ProbeProps::opacity_row());

    // 必填位 = 「字段不可空」，来源位 = 「剥掉可空外壳后是句柄」，两者都是类型层事实。
    static_assert(LabelRow::is_required() && LabelRow::is_source());
    static_assert(SizeRow::is_required() && !SizeRow::is_source());
    static_assert(!ColorRow::is_required() && !ColorRow::is_source());
    static_assert(!OpacityRow::is_required() && OpacityRow::is_source());

    // 裸值类型逐层剥掉可空与句柄外壳：可空来源件的对外类型是 float，不是 optional<ReadOnlyState<float>>。
    static_assert(std::is_same_v<LabelRow::Bare, std::string>);
    static_assert(std::is_same_v<OpacityRow::Bare, float>);
    static_assert(std::is_same_v<ColorRow::Bare, Color>);
    static_assert(std::is_same_v<LabelRow::Owner, ProbeProps>);

    // 作用域依赖位只由回落点形态决定：令牌档与派生档为真，常量档为假。
    const auto radius = ProbeProps::radius_row();
    const auto color = ProbeProps::color_row();
    const auto hover = ProbeProps::hover_row();
    AURORA_TEST_CHECK(!radius.scope_dependent());
    AURORA_TEST_CHECK(color.scope_dependent());
    AURORA_TEST_CHECK(hover.scope_dependent());
    AURORA_TEST_CHECK_EQ(std::string{color.token_name}, std::string{"theme.primary"});
    AURORA_TEST_CHECK(radius.token_name.empty());
}

AURORA_TEST_CASE(clear_is_a_compile_time_rejection_for_required_rows) {
    using LabelRow = decltype(ProbeProps::label_row());
    using SizeRow = decltype(ProbeProps::size_row());
    using RadiusRow = decltype(ProbeProps::radius_row());
    using OpacityRow = decltype(ProbeProps::opacity_row());

    // 必填行没有 clear 重载（不是运行期判空），可空行有。
    static_assert(!probe::clearable<LabelRow>);
    static_assert(!probe::clearable<SizeRow>);
    static_assert(probe::clearable<RadiusRow>);
    static_assert(probe::clearable<OpacityRow>);

    ProbeProps props;
    props.set_radius(9.0F).set_opacity(0.5F);
    AURORA_TEST_CHECK(props.is_radius_stated());
    props.clear(ProbeProps::radius_row());
    AURORA_TEST_CHECK(!props.is_radius_stated());
    AURORA_TEST_CHECK_NEAR(props.radius(), 4.0F, 1e-4F);  // 撤表态即退回常量档

    // 可空来源件撤表态后既没有源也不表态，读取落回基线。
    AURORA_TEST_CHECK(props.is_opacity_stated());
    props.clear(ProbeProps::opacity_row());
    AURORA_TEST_CHECK(!props.is_opacity_stated());
    AURORA_TEST_CHECK_NEAR(props.opacity(), 1.0F, 1e-4F);
}

AURORA_TEST_CASE(constant_fallback_row_reads_without_context) {
    ProbeProps props;
    AURORA_TEST_CHECK_NEAR(props.radius(), 4.0F, 1e-4F);  // 未表态取常量档
    props.set_radius(8.0F);
    AURORA_TEST_CHECK_NEAR(props.radius(), 8.0F, 1e-4F);
    AURORA_TEST_CHECK(props.is_radius_stated());

    // 无相等比较的类型走「不做判等短路」那一支：写入即生效，重复写入不改变结果。
    AURORA_TEST_CHECK_NEAR(props.insets().left, 1.0F, 1e-4F);
    props.set_insets(EdgeInsets{.left = 7.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    AURORA_TEST_CHECK_NEAR(props.insets().left, 7.0F, 1e-4F);
    props.set_insets(EdgeInsets{.left = 7.0F, .top = 0.0F, .right = 0.0F, .bottom = 0.0F});
    AURORA_TEST_CHECK_NEAR(props.insets().left, 7.0F, 1e-4F);
}

AURORA_TEST_CASE(token_fallback_row_follows_injected_theme_until_stated) {
    const Theme dark = Theme::dark();
    Environment dark_env;
    dark_env.set_local(dark);
    const BuildContext dark_ctx{.env = &dark_env};
    constexpr BuildContext bare_ctx;  // 未注入 ⇒ inherit_theme 兜明色基线

    ProbeProps props;
    // 未表态：同一份存储在不同作用域下读出不同生效值（解析结果没有烘进存储）。
    AURORA_TEST_CHECK(props.color(bare_ctx) == Theme::light().roles.primary);
    AURORA_TEST_CHECK(props.color(dark_ctx) == dark.roles.primary);
    AURORA_TEST_CHECK(!(props.color(dark_ctx) == props.color(bare_ctx)));

    // 表态后：读件仍要上下文，但结果不再跟随作用域。
    const Color stated{0x11, 0x22, 0x33, 0xFF};
    props.set_color(stated);
    AURORA_TEST_CHECK(props.color(dark_ctx) == stated);
    AURORA_TEST_CHECK(props.color(bare_ctx) == stated);
}

AURORA_TEST_CASE(derived_fallback_row_reads_peer_rows) {
    const Theme dark = Theme::dark();
    Environment dark_env;
    dark_env.set_local(dark);
    const BuildContext dark_ctx{.env = &dark_env};

    ProbeProps props;
    // 未表态：派生档读同族他行在当前作用域的生效值再加工。
    const Color expected{static_cast<std::uint8_t>(dark.roles.primary.r / 2),
                         static_cast<std::uint8_t>(dark.roles.primary.g / 2),
                         static_cast<std::uint8_t>(dark.roles.primary.b / 2), dark.roles.primary.a};
    AURORA_TEST_CHECK(props.hover(dark_ctx) == expected);

    // 他行表态后，派生行的回落点跟着走 —— 这正是「派生」二字的内容。
    props.set_color(Color{0x20, 0x40, 0x60, 0xFF});
    AURORA_TEST_CHECK(props.hover(dark_ctx) == Color{0x10, 0x20, 0x30, 0xFF});

    // 本行表态后不再派生。
    props.set_hover(Color{0xAA, 0xBB, 0xCC, 0xDD});
    AURORA_TEST_CHECK(props.hover(dark_ctx) == Color{0xAA, 0xBB, 0xCC, 0xDD});
}

AURORA_TEST_CASE(serialize_writes_required_always_and_stated_only) {
    ProbeProps props;
    Json out = Json::object();
    props.serialize(out);
    // 必填行恒写；未表态的可空行一个键都不写（回落语义不外泄）。
    AURORA_TEST_CHECK(out.contains("label"));
    AURORA_TEST_CHECK(out.contains("size"));
    AURORA_TEST_CHECK(!out.contains("color"));
    AURORA_TEST_CHECK(!out.contains("radius"));
    AURORA_TEST_CHECK(!out.contains("hover_color"));
    AURORA_TEST_CHECK(!out.contains("padding"));
    AURORA_TEST_CHECK(!out.contains("opacity"));

    props.set_radius(6.0F).set_label("hi");
    Json next = Json::object();
    props.serialize(next);
    AURORA_TEST_CHECK(next.contains("radius"));
    AURORA_TEST_CHECK_NEAR(next.at("radius")->as_or<float>(0.0F), 6.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(next.at("label")->as_or<std::string>(""), std::string{"hi"});

    // 撤表态后该键重新消失（与「未表态不写盘」同一条口径）。
    props.clear(ProbeProps::radius_row());
    Json again = Json::object();
    props.serialize(again);
    AURORA_TEST_CHECK(!again.contains("radius"));
}

AURORA_TEST_CASE(deserialize_writes_bound_source_without_rebinding) {
    ProbeProps props;
    State<float> opacity{0.2F};
    props.bind_opacity(opacity);
    AURORA_TEST_CHECK_NEAR(props.opacity(), 0.2F, 1e-4F);

    // 回写落进**当前源**：外部 State 的现值被改，且绑定没断（后续 set 仍驱动该属性）。
    Json empty = Json::object();
    props.deserialize(empty);
    AURORA_TEST_CHECK_NEAR(props.opacity(), 0.2F, 1e-4F);  // 键缺失保留现值

    Json in = Json::object();
    in.set("opacity", Json{0.75});
    props.deserialize(in);
    AURORA_TEST_CHECK_NEAR(opacity.get(), 0.75F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props.opacity_handle().get(), 0.75F, 1e-4F);
    opacity.set(0.9F);
    AURORA_TEST_CHECK_NEAR(props.opacity(), 0.9F, 1e-4F);

    // 必填来源件同理：绑定后回写改的是那个源。
    State<std::string> label{"origin"};
    props.bind_label(label);
    Json text = Json::object();
    text.set("label", Json{"hot-reload"});
    props.deserialize(text);
    AURORA_TEST_CHECK_EQ(label.get(), std::string{"hot-reload"});
    AURORA_TEST_CHECK_EQ(props.label(), std::string{"hot-reload"});

    // 未绑定外部源的可空行：回写按值装入并即刻表态。
    ProbeProps fresh;
    Json radius = Json::object();
    radius.set("radius", Json{9.0});
    fresh.deserialize(radius);
    AURORA_TEST_CHECK(fresh.is_radius_stated());
    AURORA_TEST_CHECK_NEAR(fresh.radius(), 9.0F, 1e-4F);
}

AURORA_TEST_CASE(deserialize_degrades_invalid_values_to_row_baseline) {
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级
    ProbeProps props;

    Json out_of_bounds = Json::object();
    out_of_bounds.set("size", Json{500.0});  // 越界（bounds 0..100）
    props.deserialize(out_of_bounds);
    AURORA_TEST_CHECK_NEAR(props.size(), 0.0F, 1e-4F);  // 必填行没有表态位：保留现值
    AURORA_TEST_CHECK(Diagnostics::count() >= 1);

    Json wrong_type = Json::object();
    wrong_type.set("radius", Json{"not-a-number"});  // 类型不符
    props.deserialize(wrong_type);
    AURORA_TEST_CHECK_NEAR(props.radius(), 4.0F, 1e-4F);  // 可空行退到该行回落基线
    AURORA_TEST_CHECK(!props.is_radius_stated());

    // 已表态的可空行吃到非法值 ⇒ 撤表态（生效值退回基线），而不是把基线伪装成用户意图写进字段。
    ProbeProps stated;
    Json good = Json::object();
    good.set("radius", Json{9.0});
    stated.deserialize(good);
    AURORA_TEST_CHECK(stated.is_radius_stated());
    stated.deserialize(wrong_type);
    AURORA_TEST_CHECK(!stated.is_radius_stated());
    AURORA_TEST_CHECK_NEAR(stated.radius(), 4.0F, 1e-4F);

    // 必填行不吃亏：一枚坏键不得把已经合法的值擦成类型默认值。
    ProbeProps kept;
    Json good_size = Json::object();
    good_size.set("size", Json{42.0});
    kept.deserialize(good_size);
    AURORA_TEST_CHECK_NEAR(kept.size(), 42.0F, 1e-4F);
    kept.deserialize(out_of_bounds);
    AURORA_TEST_CHECK_NEAR(kept.size(), 42.0F, 1e-4F);

    // 活令牌行同理：非法值后仍随档而变（写进基线即把它钉成当前档的一枚常量）。
    ProbeProps themed;
    const Theme dark = Theme::dark();
    Environment dark_env;
    dark_env.set_local(dark);
    const BuildContext ctx{.env = &dark_env};
    Json bad_color = Json::object();
    bad_color.set("color", Json{"#fff"});
    themed.deserialize(bad_color);
    AURORA_TEST_CHECK(!themed.is_color_stated());
    AURORA_TEST_CHECK(themed.color(ctx) == dark.roles.primary);
    Diagnostics::take();
}

AURORA_TEST_CASE(descriptor_entries_are_derived_from_the_table) {
    const WidgetDescriptor d = ProbeProps::describe();
    AURORA_TEST_CHECK_EQ(d.name, std::string{"Probe"});
    AURORA_TEST_CHECK_EQ(d.properties.size(), std::size_t{7});

    // 顺序 = rows() 点名顺序；发射面不再依赖字段声明序。
    const std::array<std::string_view, 7> expected_names = {"label",       "size",    "color",  "radius",
                                                            "hover_color", "padding", "opacity"};
    std::size_t index = 0;
    for (const auto expected_name : expected_names) {
        AURORA_TEST_CHECK_EQ(d.properties[index].name, std::string{expected_name});
        ++index;
    }
    AURORA_TEST_CHECK_EQ(index, d.properties.size());

    AURORA_TEST_CHECK(d.properties[0].required);
    AURORA_TEST_CHECK(!d.properties[2].required);
    AURORA_TEST_CHECK_EQ(d.properties[0].note, std::string{"Row label"});
    AURORA_TEST_CHECK_EQ(d.properties[1].min_value, std::string{"0"});
    AURORA_TEST_CHECK_EQ(d.properties[1].max_value, std::string{"100"});
    AURORA_TEST_CHECK_EQ(d.properties[1].requires_props.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(d.properties[1].requires_props[0], std::string{"label"});
    AURORA_TEST_CHECK_EQ(d.properties[1].json_type, std::string{"number"});
    AURORA_TEST_CHECK_EQ(d.properties[6].type, std::string{"float"});

    // 默认值表示按档分化：常量档给原生值（schema 里 `default` 保持数字），令牌档给带引号的令牌短名，
    // 派生档只能给显示串；必填行没有默认值可说。
    AURORA_TEST_CHECK(d.properties[3].default_native.is_number());
    AURORA_TEST_CHECK_NEAR(d.properties[3].default_native.as_or<float>(0.0F), 4.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(d.properties[2].default_value, std::string{"\"theme.primary\""});
    AURORA_TEST_CHECK(d.properties[2].default_native.is_null());
    AURORA_TEST_CHECK_EQ(d.properties[4].default_value, std::string{"darkened color"});
    AURORA_TEST_CHECK(d.properties[0].default_value.empty());
}

AURORA_TEST_CASE(signal_collection_registers_stated_sources_only) {
    ProbeProps props;
    State<std::string> label{"a"};
    props.bind_label(label);
    StateSink sink;
    props.collect(sink);
    // 必填来源件恒有源；可空来源件未表态则没有源可订。
    AURORA_TEST_CHECK_EQ(sink.sources().size(), std::size_t{1});

    props.set_opacity(0.4F);
    StateSink stated_sink;
    props.collect(stated_sink);
    AURORA_TEST_CHECK_EQ(stated_sink.sources().size(), std::size_t{2});
}

// ===========================================================================
// 宿主接线位与移动面（第 7 条「宿主绑定与移动」，对应验收判据「移动与绑定」）
// ===========================================================================

/// @brief 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
///
/// 用现成的公共观察件而不是新加接口：`mark_needs_layout()` 走 `request_frame(true)`、
/// `mark_needs_paint()` 走 `request_frame(false)`，未入树的控件同样会在自己身上回调（无父可溯时
/// `top == this`）。该回调是 `Widget` 的成员，随控件一起移动 ⇒ 移动前后观察到的是同一对计数器。
struct DirtyCounter {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到夹具控件上。
    /// @param target 待观察的控件。
    auto attach_to(ProbeWidget &target) -> void {
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

AURORA_TEST_CASE(unmounted_but_bound_widget_marks_per_tier) {
    DirtyCounter dirty;
    auto widget = ProbeWidget();  // 已 bind、未入树
    dirty.attach_to(widget);

    widget.set_radius(8.0F);  // 常量档行，档位 = 布局
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);

    widget.set_color(Color{0x11, 0x22, 0x33, 0xFF});  // 令牌档行，档位 = 绘制
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);

    widget.set_size(30.0F);  // 双档行：两档各落一次
    AURORA_TEST_CHECK_EQ(dirty.layouts, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints, 2);

    widget.set_radius(8.0F);  // 同值写入：判等短路挡住，不落账
    widget.set_size(30.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints, 2);
    AURORA_TEST_CHECK_NEAR(widget.radius(), 8.0F, 1e-4F);
}

AURORA_TEST_CASE(whole_widget_move_repoints_the_link_to_the_new_host) {
    DirtyCounter dirty;
    auto first = ProbeWidget(ProbeProps().set_radius(3.0F).set_color(Color{0x01, 0x02, 0x03, 0xFF}));
    dirty.attach_to(first);
    // 构造装入零落账：链式 DTO 那段写入发生在控件存在之前，且 `on_dirty` 装的时点在构造之后。
    AURORA_TEST_CHECK_EQ(dirty.layouts, 0);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);

    first.set_radius(4.0F);  // 落一次布局档（4.0 也是常量档回落值，但字段未表态 ⇒ 与表态后的 3.0 不同值）
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);

    auto second = ProbeWidget(std::move(first));  // ① 整控件移动
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);  // 移动本身不落账
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    AURORA_TEST_CHECK_NEAR(second.radius(), 4.0F, 1e-4F);  // 字段值随控件搬过来了
    second.set_radius(5.0F);  // 必须命中**新**宿主
    AURORA_TEST_CHECK_EQ(dirty.layouts, 2);
    second.set_color(Color{0x0A, 0x0B, 0x0C, 0xFF});
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);

    auto third = ProbeWidget(std::move(second));  // ② 连续二次移动
    third.set_radius(6.0F);
    third.set_label("moved");
    AURORA_TEST_CHECK_EQ(dirty.layouts, 3);
    AURORA_TEST_CHECK_EQ(dirty.paints, 2);

    auto target = ProbeWidget();  // ③ 移动赋值：目标原接线让位给搬进来的那条
    const int layouts_before = dirty.layouts;
    const int paints_before = dirty.paints;
    target = std::move(third);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before);
    target.set_radius(7.0F);
    target.set_hover(Color{0x20, 0x20, 0x20, 0xFF});
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before + 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before + 1);

    // 移动来源脱钩：给已被搬空的控件重新挂上观察回调再写，仍不落账（链接位的宿主已置空）。
    dirty.attach_to(third);
    third.set_radius(9.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_before + 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before + 1);
}

AURORA_TEST_CASE(node_wraps_a_widget_without_losing_the_marking) {
    // `Node(W&&)` 走的是 `make_shared<W>(std::forward<W>(w))`：控件被移动进堆上的对象里，
    // 这是全库最常见的整控件移动。标脏必须命中堆上那只活对象，而不是栈上的临时量。
    auto node = Node(ProbeWidget(ProbeProps().set_radius(6.0F)));
    // `Node::widget()` 交回 `Widget &`，而堆上那只对象由本用例构造、类型确定为 `ProbeWidget`。
    auto &live = static_cast<ProbeWidget &>(node.widget());  // NOLINT(*-pro-type-static-cast-downcast)
    DirtyCounter dirty;
    dirty.attach_to(live);

    AURORA_TEST_CHECK_NEAR(live.radius(), 6.0F, 1e-4F);
    live.set_radius(7.0F);
    live.set_opacity(0.5F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);
}

AURORA_TEST_CASE(props_copy_and_detached_dto_mark_nothing) {
    DirtyCounter dirty;
    auto host = ProbeWidget();
    dirty.attach_to(host);

    // 拷贝不携带绑定：从活控件上切一份属性对象出来，得到的是另一份未挂载 DTO。
    auto clone = ProbeProps(host);
    clone.set_radius(2.0F);
    clone.set_color(Color{0x05, 0x06, 0x07, 0xFF});
    AURORA_TEST_CHECK_EQ(dirty.layouts, 0);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    AURORA_TEST_CHECK_NEAR(clone.radius(), 2.0F, 1e-4F);  // 值照样写得进去，只是没人被标脏

    // 对照：同一批写入经已接线的控件必须落账（证明上面那条不是因为夹具根本没接线）。
    host.set_radius(2.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);

    // 脱离宿主的链式 DTO：写入只落字段，零落账。
    auto dto = ProbeProps().set_radius(3.5F).set_size(12.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    AURORA_TEST_CHECK_NEAR(dto.radius(), 3.5F, 1e-4F);
}

AURORA_TEST_CASE(resubscribe_channel_follows_the_moved_host) {
    // 换源后的重订阅经同一枚链接位调宿主的受保护件（不用 std::function 转调 ⇒ 零堆分配）。
    // 这里先移动、后挂载，再换源：来源变更的标脏必须落在移动目标上。
    State<float> opacity{0.5F};
    auto first = ProbeWidget();
    auto moved = ProbeWidget(std::move(first));
    DirtyCounter dirty;
    dirty.attach_to(moved);
    constexpr BuildContext bare_ctx;
    moved.mount(bare_ctx);
    // `StateSink::watch()` 给每条订阅都首跑一次（`run_first`），故挂载与每次重订阅都会按「当时的来源数」
    // 标一轮双档脏。计数因此不为零、也不只反映本行档位 —— 基线取在换源之前，断言只看增量。
    const int paints_after_mount = dirty.paints;
    const int layouts_after_mount = dirty.layouts;

    moved.bind_opacity(opacity);  // 换源 ⇒ 重订阅（按新来源数首跑一轮）＋ 本行档位标脏
    AURORA_TEST_CHECK(dirty.paints > paints_after_mount);
    AURORA_TEST_CHECK(dirty.layouts > layouts_after_mount);
    const int paints_after_rebind = dirty.paints;
    const int layouts_after_rebind = dirty.layouts;

    // 订阅确实重建到了移动目标上：来源变更恰标一次双档。链接位若仍指着搬空的那只控件，这里会一分不落
    // （那只控件未挂载，重订阅本就是空操作）。
    opacity.set(0.9F);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_rebind + 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_rebind + 1);

    moved.unmount();  // 先退订再让局部对象按逆序析构
}

AURORA_TEST_CASE(moved_from_source_destruction_keeps_the_new_host_wired) {
    // 「一条链接位同时只被一只控件引用」不是自律，而是两端移动面各自清零的来源侧。这条用例走真会
    // 出事的那条析构序：目标控件先声明、来源控件在内层块里后声明 ⇒ 来源**先**析构。若来源析构时仍
    // 认着自己让出去的那条链接，它会在这一刻把目标控件的接线清掉，目标此后写什么都不标脏。
    DirtyCounter dirty;
    auto target = ProbeWidget();
    {
        auto source = ProbeWidget();
        target = std::move(source);  // 移动赋值：目标的旧接线脱钩，再接管来源那条链接
        dirty.attach_to(target);  // `on_dirty` 也是 `Widget` 的成员、随移动换人 ⇒ 移动之后再挂
        target.set_radius(5.0F);
        AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    }  // `source` 在此析构：它已被搬空（槽与链接位双双归零），故析构不动目标控件的接线
    target.set_radius(6.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 2);
    target.set_color(Color{0x01, 0x02, 0x03, 0xFF});
    AURORA_TEST_CHECK_EQ(dirty.paints, 1);
}

}  // namespace aurora::test_cases::utest_props
