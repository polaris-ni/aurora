// 序列化 demo：to_json / diff / apply_patch（不对不可重建控件做 from_json 往返）。
#include "demo_common.h"

auto main() -> int {
    au::serialization::register_core_widgets();

    au::Node a = au::Column{ au::Text{ au::LocalizedString{ "hello" } }, au::Text{ au::LocalizedString{ "world" } } };
    au::Node b = au::Column{ au::Text{ au::LocalizedString{ "hello" } }, au::Text{ au::LocalizedString{ "aurora" } } };

    au::Json ja = au::serialization::to_json(a.widget());
    const au::Json jb = au::serialization::to_json(b.widget());
    const auto d = au::serialization::diff(ja, jb);
    au::serialization::apply_patch(ja, d);

    au::Node root = au::Column{
        GradientTitle{ "Serialization" },
        gap(12),
        au::Text{ au::LocalizedString{ "to_json → diff → apply_patch" } },
        au::Text{ au::LocalizedString{ "patch ops = " + std::to_string(d.size()) } },
        au::Text{
            au::LocalizedString{ "WidgetRegistry type count = " +
                                 std::to_string(au::serialization::WidgetRegistry::instance().list_types().size()) } },
    };
    return run_demo(Card{ std::move(root) }, "Serialization · Aurora Demo", 520.0f, 420.0f);
}
