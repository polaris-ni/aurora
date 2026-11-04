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

}  // namespace probe

using probe::ProbeProps;

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

}  // namespace aurora::test_cases::utest_props
