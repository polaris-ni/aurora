// Dismissible 拖动消除 demo（对照 Flutter Dismissible）。
//
// 列表卡片沿水平方向拖出（跟手 1:1 映射 + 途中渐隐），松手按阈值裁决：低于阈值 spring
// 回位、高于阈值 spring 飞出并从列表摘除（重排）。第三张卡片注册 on_dismissed 自定义
// 回调，演示「回调接管默认摘除」的扩展点（如同步删除列表数据后重建子树）。
//
// 用 Application（而非 run_demo）：Dismissible 的 spring 阶段由每帧 gesture tick 推进，
// Application::tick 内建驱动控件树的 tick_gestures，run_demo 的静态循环不含该驱动。
//
// UI 构建与 E2E 场景**同源**：已抽取到 scenes/scene_dismissible.h；`Application` 装配
// 留在本文件，E2E 用例以同一构建函数取得根节点后经驱动内核自行推进。
#include "aurora/app/application.h"
#include "demo_common.h"
#include "scenes/scene_dismissible.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    au::Scene scene{aurora::demo_scenes::build_dismissible()};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 520.0F, .height = 420.0F};
    opts.title = "Dismissible · Aurora Demo";
    auto win_res = create_native_window(opts);
    au::Application app{std::move(scene), win_res ? std::move(win_res.value()) : nullptr, opts};
    app.run();
    return 0;
}
