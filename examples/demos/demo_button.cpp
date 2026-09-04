// Button 控件 demo：点击按钮触发 on_click 回调，修改共享 State 并实时刷新计数显示。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto counter = std::make_shared<au::State<int>>(0);
    // 计数显示与计数器共享同一响应式 State：点击事件里更新它，每帧重绘即反映最新值，
    // 否则计数文本在建树时一次性求值后永不更新，点击看似「没有反应」。
    auto count_text = std::make_shared<au::State<au::LocalizedString>>(au::LocalizedString{"count = 0"});

    const auto apply = [counter, count_text](int next) -> void {
        counter->set(next);
        count_text->set(au::LocalizedString{"count = " + std::to_string(next)});
        AURORA_LOG_INFO("demo_button", "count = " + std::to_string(next));
        printf("%d", next);
    };

    au::Button b_minus{au::ButtonProps{.label = au::LocalizedString{"-1"}}};
    b_minus.on_click = [counter, apply]() -> void { apply(counter->get() - 1); };
    au::Button b_plus{au::ButtonProps{.label = au::LocalizedString{"+1"}}};
    b_plus.on_click = [counter, apply]() -> void { apply(counter->get() + 1); };
    au::Button b_reset{au::ButtonProps{.label = au::LocalizedString{"reset"}}};
    b_reset.on_click = [apply]() -> void { apply(0); };

    au::Row btn_row{
        std::move(b_minus),
        std::move(b_plus),
        std::move(b_reset),
        au::Text{au::TextProps{.content = au::Reactive{count_text}}},
    };
    btn_row.set_gap(12);  // 相邻按钮间的水平间距（主轴为水平，真正的 gap）

    au::Node root = au::Column{
        // GradientTitle{ "Button widget" },
        // gap(100),
        au::Text{au::LocalizedString{"Click button to change counter (visible in run log)"}},
        std::move(btn_row),
    };
    return run_demo(Card{std::move(root)}, "Button · Aurora Demo", 520.0F, 380.0F);
}