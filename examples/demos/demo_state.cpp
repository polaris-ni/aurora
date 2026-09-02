// State / 响应式 demo：State / Computed / Binding / Store / Action。
#include "demo_common.h"

auto main() -> int {
    auto counter = std::make_shared<au::State<int>>(3);
    const au::Computed<std::string> status{ [counter]() -> std::basic_string<char> {
        return "count = " + std::to_string(counter->get());
    } };
    au::State<std::string> name_state{ "Ada" };
    const au::Binding name{ name_state };

    auto todos = au::make_store<std::vector<std::string>>(
        std::vector<std::string>{ "alpha", "beta" },
        [](const std::vector<std::string> &s, const au::Action &a) -> std::vector<std::string> {
            if (a.type == "add") {
                if (const auto *v = a.payload_as<std::string>()) {
                    auto next = s;
                    next.push_back(*v);
                    return next;
                }
            }
            return s;
        });

    au::Button up{ au::ButtonProps{ .label = au::LocalizedString{ "State +1" } } };
    up.on_click = [counter]() -> void { counter->set(counter->get() + 1); };

    au::Button add{ au::ButtonProps{ .label = au::LocalizedString{ "Store add" } } };
    add.on_click = [todos]() -> void { todos->dispatch(au::Action{ "add", std::string{ "gamma" } }); };

    au::Node root = au::Column{
        GradientTitle{ "State / Reactivity" },
        gap(12),
        au::Text{ au::LocalizedString{ status.get() } },
        std::move(up),
        gap(8),
        au::Text{ au::LocalizedString{ "Binding name = " + name.get() } },
        au::Text{ au::LocalizedString{ "Store size = " + std::to_string(todos->get_state().size()) } },
        std::move(add),
    };
    return run_demo(Card{ std::move(root) }, "State · Aurora Demo", 520.0f, 460.0f);
}
