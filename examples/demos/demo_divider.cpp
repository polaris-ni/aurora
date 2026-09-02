// Divider 控件 demo：分隔线（横向/纵向）+ 缩进。
#include "demo_common.h"

auto main() -> int {
    au::Node root = au::Column{
        GradientTitle{ "Divider 控件" },
        gap(12),
        au::Text{ au::LocalizedString{ "列表项 A" } },
        au::Divider{ au::DividerProps{ .indent = 16.0f, .end_indent = 16.0f } },
        au::Text{ au::LocalizedString{ "列表项 B（带左右缩进）" } },
        gap(12),
        au::Text{ au::LocalizedString{ "左右两列，中间纵向分隔线：" } },
        au::Row{
            au::Text{ au::LocalizedString{ "左列内容" } },
            au::Divider{ au::DividerProps{ .orientation = au::Orientation::Vertical, .thickness = 2.0f } },
            au::Text{ au::LocalizedString{ "右列内容" } },
        },
    };
    return run_demo(Card{ std::move(root) }, "Divider · Aurora Demo", 480.0f, 380.0f);
}
