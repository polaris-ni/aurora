// Repeater 控件 demo：由 State<T> 驱动的动态列表。
#include "demo_common.h"

namespace {
struct Todo {
    int id = 0;
    std::string title;
    bool done = false;
};
}  // namespace

static auto todo_reducer(const std::vector<Todo> &s, const au::Action &a) -> std::vector<Todo> {
    if (a.type == "add") {
        if (const auto *t = a.payload_as<std::string>()) {
            auto next = s;
            next.push_back(Todo{.id = static_cast<int>(next.size()) + 1, .title = *t, .done = false});
            return next;
        }
    }
    return s;
}

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    auto todos = au::make_store<std::vector<Todo>>(std::vector{Todo{.id = 1, .title = "Explore widgets", .done = false},
                                                               Todo{.id = 2, .title = "Try theming", .done = true}},
                                                   todo_reducer);
    const auto signal = todos->as_signal();

    au::Button b_add{au::ButtonProps{.label = au::LocalizedString{"add 'new task'"}}};
    b_add.on_click = [todos]() -> void { todos->dispatch(au::Action{"add", std::string{"new task"}}); };

    au::Node root = au::Column{
        GradientTitle{"Repeater widget"},
        gap(12),
        au::Text{au::LocalizedString{"List driven by State<vector<T>>"}},
        au::Repeater<Todo>(signal,
                           [](const Todo &t, int) -> au::Row {
                               const std::string mark = t.done ? "[x] " : "[ ] ";
                               return au::Row{au::Text{au::LocalizedString{mark + t.title}}};
                           }),
        std::move(b_add),
    };
    return run_demo(Card{std::move(root)}, "Repeater · Aurora Demo", 520.0F, 460.0F);
}