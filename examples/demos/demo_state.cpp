// State / 响应式 demo：按值 State 句柄、computed 派生与新值提交。
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    auto counter = au::State<int>{3};
    auto name = au::State<std::string>{"Ada"};
    auto todos = au::State<std::vector<std::string>>{std::vector<std::string>{"alpha", "beta"}};

    // 派生按值捕获上游句柄：上游提交后读取即得新字符串，无需手写中转状态与回调。
    const auto status = au::computed(
        [counter]() -> au::LocalizedString { return au::LocalizedString{"count = " + std::to_string(counter.get())}; });
    const auto name_line =
        au::computed([name]() -> au::LocalizedString { return au::LocalizedString{"name = " + name.get()}; });
    const auto size_line = au::computed([todos]() -> au::LocalizedString {
        return au::LocalizedString{"list size = " + std::to_string(todos.get().size())};
    });
    // 值构造：只取构造瞬间的快照，之后是常量来源；与上面三行「共享句柄 + 派生」对照。
    const auto frozen = au::LocalizedString{"frozen = " + std::to_string(counter.get())};

    au::Button up{au::ButtonProps{.label = au::LocalizedString{"State +1"}}};
    up.on_click = [counter]() -> void { counter.update([](const int &value) -> int { return value + 1; }); };

    au::Button add{au::ButtonProps{.label = au::LocalizedString{"List append"}}};
    add.on_click = [todos]() -> void {
        todos.update([](std::vector<std::string> next) -> std::vector<std::string> {
            next.push_back("gamma");
            return next;
        });
    };

    au::Node root = au::Column{
        GradientTitle{"State / Reactivity"},
        gap(12),
        au::Text{au::TextProps{.content = status}},
        au::Text{au::TextProps{.content = frozen}},
        std::move(up),
        gap(8),
        au::Text{au::TextProps{.content = name_line}},
        au::Text{au::TextProps{.content = size_line}},
        std::move(add),
    };
    return run_demo(Card{std::move(root)}, "State · Aurora Demo", 520.0F, 460.0F);
}
