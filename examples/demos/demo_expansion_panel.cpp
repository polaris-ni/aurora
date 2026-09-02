// ExpansionPanel 折叠面板 demo：点击标题头展开/收起内容。
#include "demo_common.h"

auto main() -> int {
    au::ExpansionPanel details{ "Details",
                                au::Node{ au::Column{
                                    au::Text{ "Version: " AURORA_VERSION_STRING },
                                    au::Text{ "Render: software Painter (no GPU)" },
                                    au::Text{ "Backend: Headless / Win32 / GLFW" },
                                } },
                                true };
    details.set_on_toggle([](bool open) -> void { AURORA_LOG_INFO("demo", "Details: ", open ? "Expand" : "Collapse"); });

    au::ExpansionPanel advanced{ "Advanced settings", au::Node{ au::Text{ "Advanced settings content here" } }, false };

    au::Node root = au::Column{
        GradientTitle{ "ExpansionPanel collapsible panel" }, gap(12), std::move(details), gap(8), std::move(advanced),
    };
    return run_demo(Card{ std::move(root) }, "ExpansionPanel · Aurora Demo", 480.0f, 400.0f);
}
