// UI 工厂层 demo：用 au::ui:: 声明式工厂快速搭建 UI，并打印富格式 dump_tree_rich。
#include "aurora/core/log.h"
#include "aurora/ui/factories.h"

#include "demo_common.h"

auto main() -> int {
    au::Column root;
    au::ui::label(root, "Aurora UI 工厂层 Demo");
    au::ui::label(root, "下方控件由 au::ui:: 工厂自动加入根容器");
    au::ui::button(root, "点我", au::ButtonProps{}, []() -> void { /* 演示：点击无副作用 */ });
    au::ui::input(root, "可编辑文本");
    au::ui::checkbox(root, au::Reactive{ true });
    au::ui::slider(root, au::Reactive{ 0.5 });

    // 包装为根节点并赋予 #id，演示富格式 dump
    auto root_node = au::Node{ std::make_shared<au::Column>(std::move(root)) };
    root_node.set_id("demo-root");

    // 富格式文本化（供 AI 解析 / diff / 定位）
    const std::string tree = au::dump_tree_rich(root_node);
    AURORA_LOG_RAW("%s\n", tree.c_str());

    return run_demo(Card{ std::move(root_node) }, "UI Factory · Aurora Demo", 480.0f, 420.0f);
}
