// ToolBar / StatusBar 控件 demo：顶部工具栏 + 底部状态栏的经典桌面布局。
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    au::Button run_btn{au::ButtonProps{.label = "Run"}};
    run_btn.on_click = []() -> void { AURORA_LOG_INFO("demo", "Run"); };
    au::Button stop_btn{au::ButtonProps{.label = "Stop"}};

    std::vector<au::Node> tool_kids;
    tool_kids.emplace_back(std::move(run_btn));
    tool_kids.emplace_back(std::move(stop_btn));
    au::ToolBar toolbar{std::move(tool_kids)};

    std::vector<au::Node> status_kids;
    status_kids.emplace_back(au::Text{"Ready"});
    status_kids.emplace_back(au::Text{"UTF-8"});
    status_kids.emplace_back(au::Text{"Ln 1, Col 1"});  // 尾项自动右对齐
    au::StatusBar statusbar{std::move(status_kids)};

    au::Node root = au::Column{
        std::move(toolbar),
        gap(8),
        GradientTitle{"ToolBar / StatusBar"},
        au::Text{"Top toolbar + bottom status bar"},
        gap(8),
        std::move(statusbar),
    };
    return run_demo(std::move(root), "ToolBar · Aurora Demo", 560.0F, 360.0F);
}