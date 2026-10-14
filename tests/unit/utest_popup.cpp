/// 测试类型: unit
/// 目标单元: include/aurora/widget/popup.h
/// 测试说明: 覆盖 Popup——关闭/打开态绘制、布局（零尺寸占流 + 内容实测尺寸）、
/// hit_test/hit_test_chain 全局锚点重映射（内容内/内容外）、handle_outside_click
/// 三分支（未开 / 内容内 / 内容外 + dismiss 开关）、open/close 焦点作用域安全（无
/// FocusManager 直跑）、序列化往返、自描述；OverlayHost——空宿主 add_overlay 拒收、
/// 浮层增删计数、z-order 绘制与命中、顶层浮层外部点击关闭。

#include <memory>
#include <string>
#include <utility>

#include "aurora/core/types.h"
#include "aurora/environment/build_context.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/popup.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_popup {
using aurora::testing::require_field;

namespace {

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 80x40 固定尺寸弹出内容（px 意图尺寸，与字体度量无关）。
auto make_menu() -> Node {
    const auto t = std::make_shared<Text>("menu");
    t->width(aurora::px(80.0F));
    t->height(aurora::px(40.0F));
    return Node{t};
}

}  // namespace

AURORA_TEST_CASE(popup_defaults_and_descriptor) {
    const Popup p;
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "Popup");
    AURORA_TEST_CHECK_FALSE(p.is_open());
    AURORA_TEST_CHECK_TRUE(p.dismiss_on_outside_click());

    const auto d = Popup::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Popup");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
    bool has_open = false;
    for (const auto &prop : d.properties) {
        if (std::string{prop.name} == "open") {
            has_open = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_open);
    AURORA_TEST_CHECK_EQ(d.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_close");
}

AURORA_TEST_CASE(popup_open_close_lifecycle_and_paint) {
    Painter painter;
    constexpr BuildContext ctx;

    // 关闭态：绘制早返回、内容盒零尺寸。
    Popup closed{make_menu()};
    closed.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 0.0F, .height = 0.0F}},
                 ctx);
    AURORA_TEST_CHECK_EQ(closed.content_bounds().size.width, 0.0F);

    // 打开：无 FocusManager 时 open_at 安全直跑（跳过焦点作用域）。
    int close_calls = 0;
    Popup popup{make_menu()};
    popup.set_on_close([&close_calls] { ++close_calls; });
    popup.open_at(Point{.x = 100.0F, .y = 50.0F});
    AURORA_TEST_CHECK_TRUE(popup.is_open());
    AURORA_TEST_CHECK_EQ(popup.anchor().x, 100.0F);

    // 布局：常规流零尺寸，内容盒为实测内容尺寸。
    LayoutEngine::layout(popup, bounded(400.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(popup.content_bounds().size.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(popup.content_bounds().size.height, 40.0F, 1e-4F);

    // 打开态绘制：投影 + 内容绘制（绘制在锚点处）。
    popup.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 0.0F, .height = 0.0F}}, ctx);

    popup.close();
    AURORA_TEST_CHECK_FALSE(popup.is_open());
    AURORA_TEST_CHECK_EQ(close_calls, 1);
    popup.close();  // 已关闭：不重复触发回调
    AURORA_TEST_CHECK_EQ(close_calls, 1);
}

AURORA_TEST_CASE(popup_hit_test_remaps_to_anchor_content) {
    Popup popup{make_menu()};
    popup.open_at(Point{.x = 100.0F, .y = 50.0F});
    LayoutEngine::layout(popup, bounded(400.0F, 300.0F));
    constexpr BuildContext ctx;
    constexpr Rect self_box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 0.0F, .height = 0.0F}};

    // local (100,50) → 全局 (100,50) 落在内容盒 [100,180)x[50,90) 内 → 命中内容。
    Widget *inside = popup.hit_test(Point{.x = 100.0F, .y = 50.0F}, self_box, ctx);
    AURORA_TEST_CHECK_NOT_NULL(inside);

    // 命中链同样非空且以内容为终点。
    const auto chain = popup.hit_test_chain(Point{.x = 100.0F, .y = 50.0F}, self_box, ctx);
    AURORA_TEST_CHECK_FALSE(chain.empty());

    // 内容盒外 → 不命中。
    Widget *outside = popup.hit_test(Point{.x = 300.0F, .y = 250.0F}, self_box, ctx);
    AURORA_TEST_CHECK_NULL(outside);
    const auto chain_out = popup.hit_test_chain(Point{.x = 300.0F, .y = 250.0F}, self_box, ctx);
    AURORA_TEST_CHECK_TRUE(chain_out.empty());
}

AURORA_TEST_CASE(popup_outside_click_three_branches) {
    // 未打开：恒不消费。
    Popup closed;
    AURORA_TEST_CHECK_FALSE(closed.handle_outside_click(Point{.x = 1.0F, .y = 1.0F}));

    // 打开 + dismiss 开启：内容内不关，内容外关闭并消费。
    Popup popup{make_menu()};
    popup.open_at(Point{.x = 100.0F, .y = 50.0F});
    LayoutEngine::layout(popup, bounded(400.0F, 300.0F));
    AURORA_TEST_CHECK_FALSE(popup.handle_outside_click(Point{.x = 150.0F, .y = 70.0F}));  // 内容内
    AURORA_TEST_CHECK_TRUE(popup.is_open());
    AURORA_TEST_CHECK_TRUE(popup.handle_outside_click(Point{.x = 300.0F, .y = 250.0F}));  // 内容外
    AURORA_TEST_CHECK_FALSE(popup.is_open());

    // 关闭 dismiss 后：内容外点击不关闭、不消费。
    popup.set_dismiss_on_outside_click(false);
    popup.open_at(Point{.x = 100.0F, .y = 50.0F});
    LayoutEngine::layout(popup, bounded(400.0F, 300.0F));
    AURORA_TEST_CHECK_FALSE(popup.handle_outside_click(Point{.x = 300.0F, .y = 250.0F}));
    AURORA_TEST_CHECK_TRUE(popup.is_open());
    AURORA_TEST_CHECK_FALSE(popup.dismiss_on_outside_click());
}

AURORA_TEST_CASE(popup_serialize_deserialize_roundtrip) {
    Popup src{make_menu()};
    src.open_at(Point{.x = 12.0F, .y = 34.0F});
    src.set_dismiss_on_outside_click(false);

    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "open"), true);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "dismiss_on_outside_click"), false);

    Popup dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.is_open());
    AURORA_TEST_CHECK_EQ(dst.anchor().x, 12.0F);
    AURORA_TEST_CHECK_EQ(dst.anchor().y, 34.0F);
    AURORA_TEST_CHECK_FALSE(dst.dismiss_on_outside_click());
}

AURORA_TEST_CASE(overlay_host_add_remove_and_zorder) {
    // 空宿主（无基础内容）：追加的节点落在序号 0（恒为基础内容），返回 nullopt。
    OverlayHost empty;
    AURORA_TEST_CHECK_EQ(empty.overlay_count(), 0U);
    AURORA_TEST_CHECK_FALSE(empty.add_overlay(Node{std::make_shared<Popup>()}).has_value());
    empty.remove_overlay(0);  // 序号 0 恒为基础内容：拒收无副作用

    // 有基础内容：浮层序号从 1 起，可增删。
    auto base = std::make_shared<Text>("base");
    base->width(aurora::px(200.0F));
    base->height(aurora::px(100.0F));
    OverlayHost host{Node{base}};

    auto popup = std::make_shared<Popup>(make_menu());
    const std::size_t idx = aurora::testing::require_value(host.add_overlay(Node{popup}));
    AURORA_TEST_CHECK_EQ(idx, 1U);
    AURORA_TEST_CHECK_EQ(host.overlay_count(), 1U);

    host.layout(bounded(400.0F, 300.0F), BuildContext{});

    // z-order 绘制：基础内容在下、浮层在上（打开态绘制路径）。
    popup->open_at(Point{.x = 10.0F, .y = 10.0F});
    host.layout(bounded(400.0F, 300.0F), BuildContext{});
    Painter painter;
    host.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = host.size()}, BuildContext{});

    // 顶层浮层外部点击关闭。
    AURORA_TEST_CHECK_TRUE(host.handle_outside_click(Point{.x = 300.0F, .y = 250.0F}));
    AURORA_TEST_CHECK_FALSE(popup->is_open());

    // 移除浮层（越界序号无副作用）。
    host.remove_overlay(5U);
    host.remove_overlay(idx);
    AURORA_TEST_CHECK_EQ(host.overlay_count(), 0U);
    AURORA_TEST_CHECK_EQ(std::string{host.type_name()}, "OverlayHost");
    const auto d = OverlayHost::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    AURORA_TEST_CHECK_TRUE(d.properties.empty());
}

}  // namespace aurora::test_cases::utest_popup
