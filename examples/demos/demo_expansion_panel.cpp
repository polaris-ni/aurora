// ExpansionPanel 折叠面板 demo：点击标题头展开/收起内容。
#include "demo_common.h"

auto main() -> int {
    au::ExpansionPanel details{ "详细信息",
                                au::Node{ au::Column{
                                    au::Text{ "版本: 1.0.0.alpha-1" },
                                    au::Text{ "渲染: 软件 Painter（无 GPU）" },
                                    au::Text{ "后端: Headless / Win32 / GLFW" },
                                } },
                                true };
    details.set_on_toggle([](bool open) -> void { AURORA_LOG_INFO("demo", "详细信息: ", open ? "展开" : "收起"); });

    au::ExpansionPanel advanced{ "高级设置", au::Node{ au::Text{ "这里是高级设置内容" } }, false };

    au::Node root = au::Column{
        GradientTitle{ "ExpansionPanel 折叠面板" }, gap(12), std::move(details), gap(8), std::move(advanced),
    };
    return run_demo(Card{ std::move(root) }, "ExpansionPanel · Aurora Demo", 480.0f, 400.0f);
}
