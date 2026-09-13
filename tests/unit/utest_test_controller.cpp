/// 测试类型: unit
/// 目标单元: include/aurora/app/test_controller.h
/// 测试说明: 覆盖无头 widget 测试驱动 TestController——帧驱动（首帧渲染 / idle 帧跳过 /
/// pump_and_settle 收敛 / set_viewport 触发重排）、查找三件套（find_by_key 走 Node::set_id、
/// find_by_type 走 type_name、find_by_text 走文本类属性）、交互闭环（tap 触发点击回调、
/// enter_text 落字、drag 建立文本选区）、断言（expect_visible 正反例、expect_prop 比对与
/// 失配错误、空节点的错误返回）。

#include <memory>
#include <string>
#include <string_view>

#include "aurora/app/test_controller.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_test_controller {
/// 说明：无头后端专属用例在**用例体内**按 AURORA_BACKEND_HEADLESS 分支——宏关闭时走 SKIP 桩。
/// 同一 AURORA_TEST_CASE 名在两种配置下都必须注册，否则 check_test_registry 会判
/// 「源码有字面量但 --list 缺失」的漂移（该门禁不对预处理分支求值）。


namespace {

/// @brief 固定视口（320×240）：文本在窄视口下必须换行，便于验证 set_viewport 的重排效果。
constexpr int kWidth = 320;
constexpr int kHeight = 240;

/// @brief 测试树干：Column[ Button#ok(label="OK"), Text#msg(content="hello"), TextInput#in ]。
struct Fixture {
    std::shared_ptr<Button> btn = std::make_shared<Button>(ButtonProps{.label = LocalizedString{"OK"}});
    std::shared_ptr<Text> msg = std::make_shared<Text>(TextProps{.content = LocalizedString{"hello"}});
    std::shared_ptr<TextInput> input = std::make_shared<TextInput>();
    int clicks = 0;

    Fixture() {
        btn->on_click = [this]() -> void { ++clicks; };
        input->set_placeholder("Enter name");
    }

    /// @brief 组装根节点（每次返回新树：控制器持有所有权，用例间互不影响）。
    [[nodiscard]] auto tree() const -> Node {
        auto btn_node = Node{btn};
        btn_node.set_id("ok");
        auto msg_node = Node{msg};
        msg_node.set_id("msg");
        auto in_node = Node{input};
        in_node.set_id("in");

        auto col = std::make_shared<Column>();
        col->add(btn_node);
        col->add(msg_node);
        col->add(in_node);
        return Node{col};
    }

    [[nodiscard]] auto controller() const -> TestController {
        return TestController{tree(), TestControllerConfig{.width = kWidth, .height = kHeight}};
    }
};

}  // namespace


AURORA_TEST_CASE(pump_renders_first_frame_then_idle_frames_are_skipped) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();

    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_CHECK_EQ(tc.frame_count(), 1);  // 首帧必须真实渲染（挂载 + 布局 + 绘制）

    // 无脏来源的后续帧由 present_root 整帧跳过（headless 下不计数），与 Window 脏区语义同源。
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_CHECK_EQ(tc.frame_count(), 1);

    AURORA_TEST_REQUIRE_TRUE(tc.pump(3).ok());
    AURORA_TEST_CHECK_EQ(tc.frame_count(), 1);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(pump_and_settle_stops_before_budget) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();

    const int frames = tc.pump_and_settle(20);
    AURORA_TEST_CHECK_GE(frames, 1);  // 至少渲染首帧
    AURORA_TEST_CHECK_LT(frames, 20);  // 静态树：远未到预算即收敛（idle 帧且无动画）

    const int before = tc.frame_count();
    AURORA_TEST_CHECK_EQ(tc.pump_and_settle(20), 1);  // 已收敛：再 settle 只跑一帧即返回
    AURORA_TEST_CHECK_EQ(tc.frame_count(), before);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(set_viewport_relayouts_tree) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_CHECK_NEAR(tc.root_node().bounds().size.width, static_cast<float>(kWidth), 0.5F);

    AURORA_TEST_REQUIRE_TRUE(tc.set_viewport(640, 480).ok());
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_CHECK_NEAR(tc.root_node().bounds().size.width, 640.0F, 0.5F);
    AURORA_TEST_CHECK_NEAR(tc.root_node().bounds().size.height, 480.0F, 0.5F);

    // 非法尺寸不改视口（返回错误，树几何保持上一次有效布局）。
    AURORA_TEST_CHECK_FALSE(tc.set_viewport(0, 480).ok());
    AURORA_TEST_CHECK_FALSE(tc.set_viewport(-1, 480).ok());
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_CHECK_NEAR(tc.root_node().bounds().size.width, 640.0F, 0.5F);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(finders_locate_nodes_by_key_type_and_text) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());

    const auto ok = tc.find_by_key("ok");
    AURORA_TEST_REQUIRE_EQ(ok.size(), 1U);
    AURORA_TEST_CHECK(ok.at(0).widget().type_name() == std::string_view{"Button"});
    AURORA_TEST_CHECK(tc.find_by_key("nope").empty());

    const auto buttons = tc.find_by_type("Button");
    AURORA_TEST_CHECK_EQ(buttons.size(), 1U);
    const auto texts = tc.find_by_type("Text");
    AURORA_TEST_CHECK_EQ(texts.size(), 1U);

    // Text 的文本属性名为 content、Button 为 label——启发式 key 集二者都要命中。
    const auto by_hello = tc.find_by_text("hello");
    AURORA_TEST_REQUIRE_EQ(by_hello.size(), 1U);
    AURORA_TEST_CHECK(by_hello.at(0).id() == std::string_view{"msg"});
    const auto by_ok = tc.find_by_text("OK");
    AURORA_TEST_REQUIRE_EQ(by_ok.size(), 1U);
    AURORA_TEST_CHECK(by_ok.at(0).id() == std::string_view{"ok"});
    AURORA_TEST_CHECK(tc.find_by_text("missing").empty());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(tap_fires_on_click_and_enter_text_writes_into_input) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());

    auto ok = tc.find_by_key("ok");
    AURORA_TEST_REQUIRE_EQ(ok.size(), 1U);
    AURORA_TEST_CHECK_EQ(fx.clicks, 0);
    AURORA_TEST_REQUIRE_TRUE(tc.tap(ok.at(0)).ok());
    AURORA_TEST_CHECK_EQ(fx.clicks, 1);

    auto in = tc.find_by_key("in");
    AURORA_TEST_REQUIRE_EQ(in.size(), 1U);
    AURORA_TEST_REQUIRE_TRUE(tc.enter_text(in.at(0), "abc").ok());
    AURORA_TEST_CHECK(fx.input->value() == std::string{"abc"});

    // 交互引发标脏 → settle 帧应真实渲染（>1 帧 vs 静态树的 1 帧）。
    AURORA_TEST_CHECK_GE(tc.pump_and_settle(20), 2);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(drag_establishes_text_selection) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());

    auto msg = tc.find_by_key("msg");
    AURORA_TEST_REQUIRE_EQ(msg.size(), 1U);
    AURORA_TEST_REQUIRE_TRUE(tc.drag(msg.at(0), Point{.x = 60.0F, .y = 0.0F}).ok());
    AURORA_TEST_CHECK(fx.msg->has_selection());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(interactions_reject_empty_node) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    Node empty;
    AURORA_TEST_CHECK_FALSE(tc.tap(empty).ok());
    AURORA_TEST_CHECK_FALSE(tc.drag(empty, Point{.x = 1.0F, .y = 1.0F}).ok());
    AURORA_TEST_CHECK_FALSE(tc.enter_text(empty, "x").ok());
    AURORA_TEST_CHECK_FALSE(tc.expect_visible(empty).ok());
    AURORA_TEST_CHECK_FALSE(tc.expect_prop(empty, "show", Json{true}).ok());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(expect_visible_passes_after_frame_and_fails_when_hidden) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();

    auto msg = tc.find_by_key("msg");
    AURORA_TEST_REQUIRE_EQ(msg.size(), 1U);
    AURORA_TEST_CHECK_FALSE(tc.expect_visible(msg.at(0)).ok());  // 未 pump：无几何

    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());
    AURORA_TEST_REQUIRE_TRUE(tc.expect_visible(msg.at(0)).ok());

    // show=false 的节点不入绘制：可见性断言失败。
    fx.msg->show.set(false);
    AURORA_TEST_CHECK_FALSE(tc.expect_visible(msg.at(0)).ok());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}


AURORA_TEST_CASE(expect_prop_compares_resolved_value) {
#ifdef AURORA_BACKEND_HEADLESS
    Fixture fx;
    auto tc = fx.controller();
    AURORA_TEST_REQUIRE_TRUE(tc.pump().ok());

    auto msg = tc.find_by_key("msg");
    AURORA_TEST_REQUIRE_EQ(msg.size(), 1U);
    // 注意 Json{"hello"} 在 nlohmann 里是数组 ["hello"]，字符串期望值须显式构造。
    AURORA_TEST_REQUIRE_TRUE(tc.expect_prop(msg.at(0), "content", Json(std::string{"hello"})).ok());
    AURORA_TEST_REQUIRE_TRUE(tc.expect_prop(msg.at(0), "show", Json(true)).ok());

    const Result<void> mismatch = tc.expect_prop(msg.at(0), "content", Json(std::string{"goodbye"}));
    AURORA_TEST_CHECK_FALSE(mismatch.ok());
    AURORA_TEST_CHECK_NE(mismatch.error().message.find("goodbye"), std::string::npos);
    AURORA_TEST_CHECK_NE(mismatch.error().message.find("hello"), std::string::npos);  // 实际值也在错误里

    // 未知属性 → 缺失；与期望不等。
    AURORA_TEST_CHECK_FALSE(tc.expect_prop(msg.at(0), "no_such_prop", Json{1}).ok());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启：TestController 依赖 HeadlessSurface 未编译");
#endif
}

}  // namespace aurora::test_cases::utest_test_controller
