// Scroll 控件 demo：可滚动区域（滚轮滚动）。
//
// UI 构建与 E2E 场景**同源**：已抽取到 scenes/scene_scroll.h（inline 构建函数返回根
// Node），本文件退化为薄 main()。窗口尺寸、标题与渲染结果与抽取前逐字节一致。
#include "demo_common.h"
#include "scenes/scene_scroll.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int { return run_demo(aurora::demo_scenes::build_scroll(), "Scroll · Aurora Demo", 520.0F, 420.0F); }
