// TestController demo：无头「渲染 → 交互 → 断言」闭环。
//
// 演示 item-controller 的典型脚本：建树（给节点打 key）→ pump 首帧 → 按 key 查找 →
// 交互（点击 / 输入文本）→ pump_and_settle 收敛 → 属性断言。全程无窗口系统，
// 结果打印到 stdout（CI / 远程环境可跑）。
#include "demo_common.h"

#ifdef AURORA_BACKEND_HEADLESS

#include <cstdio>
#include <memory>
#include <string>

namespace {

auto run() -> int {
    auto btn = std::make_shared<au::Button>(au::ButtonProps{.label = au::LocalizedString{"Tap me"}});
    auto counter = std::make_shared<au::State<int>>(0);
    auto count_text = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"clicks = 0"});

    btn->on_click = [counter, count_text]() -> void {
        const int next = counter->get() + 1;
        counter->set(next);
        count_text->set(au::LocalizedString{"clicks = " + std::to_string(next)});
    };

    auto input = std::make_shared<au::TextInput>();
    input->set_placeholder("Enter name");

    // 给节点打 key：find_by_key 走 Node::set_id（须在装入容器前设置）。
    auto btn_node = au::Node{btn};
    btn_node.set_id("tap");
    auto label_node = au::Node{au::Text{au::TextProps{.content = au::Reactive{count_text}}}};
    label_node.set_id("counter");
    auto input_node = au::Node{input};
    input_node.set_id("name");

    auto col = std::make_shared<au::Column>();
    col->add(btn_node);
    col->add(label_node);
    col->add(input_node);

    au::TestController tc{au::Node{col}, au::TestControllerConfig{.width = 320, .height = 240}};

    // ① 渲染首帧（挂载 + 布局 + 绘制）。
    (void)tc.pump();
    std::printf("frames after first pump: %d\n", tc.frame_count());

    // ② 按 key / 类型 / 文本查找。
    const auto taps = tc.find_by_key("tap");
    const auto inputs = tc.find_by_key("name");
    std::printf("find_by_key(\"tap\")=%zu  find_by_key(\"name\")=%zu  find_by_type(\"Button\")=%zu\n", taps.size(),
                inputs.size(), tc.find_by_type("Button").size());

    // ③ 交互：点击 + 文本输入。
    if (!taps.empty()) {
        (void)tc.tap(taps.at(0));
        (void)tc.tap(taps.at(0));
    }
    if (!inputs.empty()) {
        (void)tc.enter_text(inputs.at(0), "Aurora");
    }

    // ④ 收敛（交互后由内部请求重新渲染，故本例会跑满若干帧）。
    const int settled = tc.pump_and_settle(20);
    std::printf("pump_and_settle frames: %d, total frames: %d\n", settled, tc.frame_count());

    // ⑤ 断言：可见性 + 属性值 + 文本内容。
    auto counters = tc.find_by_text("clicks = 2");
    std::printf("counter matches 'clicks = 2': %s\n", counters.empty() ? "no" : "yes");
    const au::Result<void> visible = au::TestController::expect_visible(taps.at(0));
    const au::Result<void> value_ok = au::TestController::expect_prop(
        inputs.at(0), "value", au::Json(std::string{"Aurora"}));
    std::printf("expect_visible(button): %s\n", visible.ok() ? "pass" : visible.error().message.c_str());
    std::printf("expect_prop(input, value='Aurora'): %s\n", value_ok.ok() ? "pass" : value_ok.error().message.c_str());

    const bool ok = !counters.empty() && visible.ok() && value_ok.ok();
    std::printf("demo %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int { return run(); }

#else

auto main() -> int {
    std::printf("demo_test_controller: AURORA_BACKEND_HEADLESS 未开启，跳过\n");
    return 0;
}

#endif
