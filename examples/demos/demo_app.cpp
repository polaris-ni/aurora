// au::App() 流式构建器演示（specification/06-app-platform.md §4）。
// 用 `au::App().title(...).size(...).view(root).run()` 一行式启动应用，
// 无需手动构造 Application / Scene / Window。
#include "aurora/window/platform.h"
#include "demo_common.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    const aurora::Platform p = aurora::platform();
    aurora::Text line1{"au::App() demo"};
    aurora::Text line2{"surface: " + std::to_string(static_cast<int>(p.surface))};
    line1.modifier.set(aurora::Modifier{}.size(240.0F, 28.0F));
    line2.modifier.set(aurora::Modifier{}.size(240.0F, 28.0F));

    auto root = aurora::Column{aurora::ColumnProps{.children = {
                                                       std::move(line1),
                                                       gap(12.0F),
                                                       std::move(line2),
                                                   }}};
    aurora::Node tree{std::move(root)};  // 单一宿主：inspector 与 App 各持一份共享句柄

#ifdef AURORA_BUILD_INSPECTOR_SERVER
    // 无人值守取证通道（opt-in：未设 AURORA_INSPECTOR_PORT 时完全静默，不影响本载体
    // 「stdout/stderr 均空」的基线差异）——本载体不经 run_demo，故自行接线同一口径。
    // Surface getter 不可得（窗口由 `App::run` 内部构造），故只提供 `/api/tree` 侧读数。
    auto inspector = start_demo_inspector([node = tree]() -> aurora::Node { return node; });
#endif

    aurora::App().title("au::App() fluent wrapper").size(420, 300).view(aurora::Node{tree}).run();
    return 0;
}