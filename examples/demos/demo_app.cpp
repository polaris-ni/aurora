// au::App() 流式构建器演示（specification/06-app-platform.md §4）。
// 用 `au::App().title(...).size(...).view(root).run()` 一行式启动应用，
// 无需手动构造 Application / Scene / Window。
#include "aurora/window/platform.h"
#include "demo_common.h"

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
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

    aurora::App().title("au::App() fluent wrapper").size(420, 300).view(std::move(root)).run();
    return 0;
}