/// 测试类型: unit
/// 目标单元: include/aurora/widget/checkbox.h
/// 测试说明: 覆盖 Checkbox——默认态、set_value 触发 on_changed、Press/Release 合成点击切换、
/// 禁用态忽略点击、共享来源双向写穿（含用户输入）与信号收集、方框尺寸布局（含主题度量档跟随
/// 与显式设值优先）、自描述、序列化往返

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/environment/environment.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/state/state.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/checkbox.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_checkbox {

using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：调用方持有 holder 即保证环境存活（BuildContext 本就只供当帧读取）。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

/// 探针：把 protected 的 `resolve_size` 提到 public（与 `on_layout` 走的是同一条函数）。
class SizeProbe final : public Checkbox {
  public:
    using Checkbox::resolve_size;
};

}  // namespace

AURORA_TEST_CASE(checkbox_defaults_and_type_name) {
    const Checkbox c;
    AURORA_TEST_CHECK_EQ(std::string{c.type_name()}, "Checkbox");
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_CHECK_TRUE(c.enabled());

    Json props = Json::object();
    c.serialize_props(props);
    // 默认未显式设边长：size 键不输出（两态语义：空 = 跟随主题 controls.indicator_size）。
    AURORA_TEST_CHECK_FALSE(props.contains("size"));
}

AURORA_TEST_CASE(checkbox_set_value_fires_on_changed) {
    std::vector<bool> seen;
    Checkbox c;
    c.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

    c.set_value(true);
    AURORA_TEST_CHECK_TRUE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    c.set_value(false);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(checkbox_pointer_press_release_toggles) {
    std::vector<bool> seen;
    Checkbox c;
    c.set_on_changed([&seen](bool v) -> void { seen.push_back(v); });

    MouseEvent press;
    press.action = MouseAction::Press;
    c.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);
    AURORA_TEST_CHECK_FALSE(c.value());  // 按下不切换

    MouseEvent release;
    release.action = MouseAction::Release;
    c.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(release.is_handled);
    AURORA_TEST_CHECK_TRUE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_TRUE(seen[0]);

    // 第二次完整点击切回 false。
    MouseEvent press2;
    press2.action = MouseAction::Press;
    c.on_pointer_event(press2);
    MouseEvent release2;
    release2.action = MouseAction::Release;
    c.on_pointer_event(release2);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_FALSE(seen[1]);
}

AURORA_TEST_CASE(checkbox_disabled_ignores_clicks) {
    int calls = 0;
    Checkbox c;
    c.set_on_changed([&calls](bool) -> void { ++calls; });
    c.set_enabled(false);
    AURORA_TEST_CHECK_FALSE(c.enabled());

    MouseEvent press;
    press.action = MouseAction::Press;
    c.on_pointer_event(press);
    AURORA_TEST_CHECK_TRUE(press.is_handled);

    MouseEvent release;
    release.action = MouseAction::Release;
    c.on_pointer_event(release);
    AURORA_TEST_CHECK_FALSE(c.value());
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(checkbox_shared_source_writes_through_to_upstream) {
    // 双向值属性：构造只接受可写句柄；只读句柄在构造期即拒绝（写回无从兑现）。
    static_assert(!std::is_constructible_v<Checkbox, ReadOnlyState<bool>>,
                  "editable value property must not accept a read-only handle");

    State<bool> const up{false};
    Checkbox c{up};  // 共享同一来源：控件与上游是同一个值
    AURORA_TEST_CHECK_FALSE(c.value());

    c.set_value(true);
    AURORA_TEST_CHECK_TRUE(up.get());  // 控件 → 上游

    up.set(false);
    AURORA_TEST_CHECK_FALSE(c.value());  // 上游 → 控件（读取穿透）

    // 用户输入同理写穿：指针点击切换落回同一来源。
    MouseEvent press;
    press.action = MouseAction::Press;
    c.on_pointer_event(press);
    MouseEvent release;
    release.action = MouseAction::Release;
    c.on_pointer_event(release);
    AURORA_TEST_CHECK_TRUE(up.get());  // 点击 → 上游

    // 信号收集：共享来源只采集一次。
    StateSink out;
    c.collect_signals(out);
    AURORA_TEST_CHECK_EQ(out.sources().size(), std::size_t{1});

    // 自有来源构造同样只收集一份。
    Checkbox plain;
    StateSink single;
    plain.collect_signals(single);
    AURORA_TEST_CHECK_EQ(single.sources().size(), std::size_t{1});
}

AURORA_TEST_CASE(checkbox_layout_uses_box_size) {
    Checkbox c;
    LayoutEngine::layout(c, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 20.0F, 1e-4F);

    c.set_size(30.0F);
    // 库运行时语义：Widget::layout 以约束为键做布局缓存（widget.cpp），set_size 不标脏，
    // 约束不变时命中缓存复用旧尺寸 20；换用不同约束强制真实重排。
    LayoutEngine::layout(c, bounded(210.0F, 110.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 30.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 30.0F, 1e-4F);

    // 约束不足时钳制。
    LayoutEngine::layout(c, bounded(25.0F, 25.0F));
    AURORA_TEST_CHECK_NEAR(c.size().width, 25.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(c.size().height, 25.0F, 1e-4F);
}

AURORA_TEST_CASE(checkbox_describe_reports_metadata) {
    const auto d = Checkbox::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Checkbox");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_changed");
    bool has_checked = false;
    for (const auto &p : d.properties) {
        if (std::string{p.name} == "checked") {
            has_checked = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_checked);
}

AURORA_TEST_CASE(checkbox_serialize_deserialize_roundtrip) {
    int calls = 0;
    Checkbox src;
    src.set_on_changed([&calls](bool) -> void { ++calls; });
    src.set_value(true);
    src.set_size(28.0F);
    src.set_enabled(false);
    AURORA_TEST_CHECK_EQ(calls, 1);  // 序列化前回调已随 set_value 触发

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "checked"), true);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "enabled"), false);

    Checkbox dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.value());
    AURORA_TEST_CHECK_FALSE(dst.enabled());
    LayoutEngine::layout(dst, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(dst.size().width, 28.0F, 1e-4F);
}

// ---- 度量跟随（接线批）：未显式设边长 → 读主题 `controls.indicator_size` ----

AURORA_TEST_CASE(checkbox_size_default_to_material_baseline) {
    // 无注入 = Material 基线档：接线前后实测同为 20dp，观感零回归的锚点。
    const BuildContext bare;
    SizeProbe const c;
    AURORA_TEST_CHECK_NEAR(c.resolve_size(bare), 20.0F, 1e-4F);
}

AURORA_TEST_CASE(checkbox_size_follows_theme_tokens_when_unset) {
    // 改过令牌的主题 ⇒ 解析值随令牌走（证明读的是 theme，而不是写死的常量）。
    Theme t = Theme::light();
    t.controls.indicator_size = 26.0F;
    const auto [env, ctx] = themed_ctx(t);
    SizeProbe c;
    AURORA_TEST_CHECK_NEAR(c.resolve_size(ctx), 26.0F, 1e-4F);
    LayoutEngine::layout(c, bounded(200.0F, 100.0F), ctx);
    AURORA_TEST_CHECK_NEAR(c.size().width, 26.0F, 1e-4F);  // 布局与绘制同源（同一条 resolve 通道）
    AURORA_TEST_CHECK_NEAR(c.size().height, 26.0F, 1e-4F);
}

AURORA_TEST_CASE(checkbox_explicit_size_overrides_theme) {
    // 对照组：显式设过边长后主题不动（两态语义的另一半）。
    Theme t = Theme::light();
    t.controls.indicator_size = 26.0F;
    const auto [env, ctx] = themed_ctx(t);
    SizeProbe c;
    c.set_size(14.0F);
    AURORA_TEST_CHECK_NEAR(c.resolve_size(ctx), 14.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_checkbox
