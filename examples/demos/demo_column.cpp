// Column 控件 demo：纵向线性布局，演示主轴/交叉轴对齐。
//
// UI 构建与 E2E 场景**同源**：已抽取到 scenes/scene_column.h（inline 构建函数返回根
// Node），本文件退化为薄 main()。窗口尺寸、标题与渲染结果与抽取前逐字节一致。
#include "demo_common.h"
#include "scenes/scene_column.h"

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int { return run_demo(aurora::demo_scenes::build_column(), "Column · Aurora Demo", 560.0F, 560.0F); }
