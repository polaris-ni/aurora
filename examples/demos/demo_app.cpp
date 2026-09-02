// au::App() 流式构建器演示（specification/06-app-platform.md §4）。
// 用 `au::App().title(...).size(...).view(root).run()` 一行式启动应用，
// 无需手动构造 Application / Scene / Window。
#include "aurora/window/platform.h"

#include "demo_common.h"

auto main() -> int {
    const aurora::Platform p = aurora::platform();
    aurora::Text line1{ "au::App() demo" };
    aurora::Text line2{ "surface: " + std::to_string(static_cast<int>(p.surface)) };
    line1.modifier.set(aurora::Modifier{}.size(240.0f, 28.0f));
    line2.modifier.set(aurora::Modifier{}.size(240.0f, 28.0f));

    auto root = aurora::Column{ aurora::ColumnProps{ .children = {
                                                         std::move(line1),
                                                         gap(12.0f),
                                                         std::move(line2),
                                                     } } };

    aurora::App().title("au::App() 流式封装").size(420, 300).view(std::move(root)).run();
    return 0;
}
