# F2. AI 工具链层：Recipe / LSP-MCP-CLI / 可逆性（#16,#17,#22）

> 本文件是「三、特性详细规范」子文档，覆盖 **§F2.**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #16 示例驱动文档（Recipe 形式）

**核心目标：** AI 从示例高效学习

**规范：**

```cpp
// 每个组件的文档 = 一个可编译运行的最小示例
// examples/button_basic.cpp
#include <aurora/aurora.h>

int main() {
    auto root = au::Column(au::ColumnProps{
        .gap = 8,
        .children = {
            std::move(au::Text("Button Examples").font_size(20).bold()),
            au::Button(au::ButtonProps{ .label = "Primary" }),
            au::Button(au::ButtonProps{ .label = "Outline" }),
            std::move(au::Button(au::ButtonProps{ .label = "Disabled" }).set_enabled(false)),
        },
    });
    root.modifier = au::Modifier{}.padding(20);  // padding 属 Widget 基类，须对构造后的对象赋值
    auto win_res = au::create_window(au::Win32Options{ .title = "Button Example", .size = {640, 480} });
    au::Application app{ au::Scene{ std::move(root) },
                         win_res ? std::move(win_res.value()) : nullptr,
                         au::WindowOptions{ .title = "Button Example", .size = {640, 480} } };
    app.run();
    return 0;
}
```

**关键约束：**

- 每个示例 **≤ 30 行**，可独立编译运行
- 示例覆盖所有组件的 **所有常见用法**
- 示例本身就是 **集成测试**
- 示例被组织成 **（GUIDELINE.md）配方形式**，AI 可以通过检索示例直接拼接出目标代码（见 `GUIDELINE.md`）

---

#### #17 LSP / MCP Server / CLI 工具链

**核心目标：** AI Agent 直接集成

**落地状态：**

- **MCP Server（aurora-mcp）**：stdio JSON-RPC 2.0，暴露 10 个 MCP tools（list_components / describe_component /
  search_components / validate_tree / render_snapshot / render_png / to_code / to_yaml / get_schema / validate_ui）。详见
  §H.17。
- **CLI 工具（aurora_cli）**：子命令：components / describe / search / validate / snapshot / render / to-code /
  to-yaml / schema。详见 §H.17。
- **LSP（aurora-lsp）**：stdio JSON-RPC 2.0 语言服务，对 `au::<Type>Props{ .prop = ... }` 等声明式写法提供
  completion / hover / diagnostics / codeAction 四件套：
    - **completion**：`au::` 后补组件/枚举类型；`XxxProps{` 内 `.` 后补属性（显示默认值/必填/文档）；枚举属性 `=` 后补枚举值。
    - **hover**：`au::Type` 显示组件概要（属性数/事件/示例）；`.prop` 显示类型/默认值/必填/文档。
    - **diagnostics**（didOpen/didChange 后 publishDiagnostics）：未知组件类型、未知属性、非法枚举值、缺失必填属性（warning）。
    - **codeAction**：为缺失必填属性的组件生成「补全缺失必填属性」快速修复（在 `}` 前插入 `.prop = <default>`）。
    - 消费库 live API（`describe_component` + `known_enums`），无需读取 `aurora_api.json` 文件，始终与代码同步。详见 §H.17。

<details>
<summary>原始目标形态（保留供参考）</summary>

**1. Language Server Protocol 实现（aurora-lsp）：**

```text
支持能力：
- textDocument/completion    → 组件名、属性名、枚举值补全
- textDocument/hover         → 显示属性类型、默认值、文档
- textDocument/definition    → 跳转到组件/属性定义
- textDocument/diagnostic    → 实时报告布局错误、类型错误
- textDocument/codeAction    → 提供自动修复建议（如 au::px(120) 替换 120）
```

**2. MCP (Model Context Protocol) Server（aurora-mcp）：**

```text
协议端点：
- GET  /api/components          → 返回所有组件的 JSON Schema 列表
- GET  /api/components/{name}   → 返回单个组件的完整描述
- POST /api/validate            → 验证 UI 树 JSON 的合法性
- POST /api/render              → 离屏渲染 UI 树，返回快照
- GET  /api/recipes?query=...   → 搜索示例代码
- WS   /ws/inspector            → 实时 UI 树订阅（变化推送）

消息格式：JSON-RPC 2.0
认证：本地开发无需认证；远程连接需 Bearer Token
```

**3. CLI 工具（aurora）：**

```bash
$ aurora new my_project        # 创建新项目脚手架
$ aurora validate main.cpp     # 检查 UI 代码正确性，输出 JSON 错误
$ aurora preview main.cpp      # 快速预览 UI（启动临时窗口）
$ aurora snapshot main.cpp     # 生成逻辑快照 + PNG 截图
$ aurora describe my_app       # 输出运行中应用的 UI 树 JSON
$ aurora ai-compat-test        # AI 兼容性测试（调用 LLM 生成代码并编译）
$ aurora schema                # 输出 aurora_api.json
$ aurora to-code tree.json     # UI 树 JSON → C++ 代码（#22）
```

</details>

---

#### #22 可逆性：UI → 代码的参考还原

**核心目标：** AI 可分析现有界面并重构

**规范：**

```cpp
// 不追求"完全可逆"，而是"结构化往返"：

// 正向：代码 → UI 树（JSON）—— 必须支持
au::Json json = au::serialization::to_json(widget_tree);   // 注意：to_json 返回 au::Json（非 std::string）

// 反向：UI 树（JSON）→ 代码 —— 提供"参考实现"而非"精确还原"
std::string code = au::to_code(json, au::CodeStyle::Fluent);
// 生成的代码是"规范形式"，不保留原始代码的变量名、注释、结构

// 运行时 → 代码（Inspector 场景）
std::string code = au::to_code(au::dump_tree_json(root));   // 见 widget/inspect.h + #22
// 用途：AI 看到运行效果 → 获取当前结构 → 生成修改代码
// 不保证与原始代码完全一致，但保证语义等价
```

**代码风格选项：**

```cpp
enum class CodeStyle {
    Fluent,         // 链式 setter：au::Button(au::ButtonProps{ .label = "OK" }).set_on_click(fn)
    StepByStep,     // 分步：auto btn = au::Button(au::ButtonProps{}); btn.label = "OK"; btn.set_on_click(fn);
    DesignatedInit, // 指定初始化器：au::ButtonProps{ .label = "OK", .on_click = fn }（仅 *Props 聚合；控件类型非聚合）
};
// 注：CodeStyle 枚举与 to_code(json, style) 重载已实现（见 widget/codegen.h）；默认 Fluent。
//   - Fluent：扁平容器 + au::Type(au::TypeProps{...}) 叶形式
//   - DesignatedInit：统一 au::TypeProps{ .prop = val, .children = {...} } 形式（*Props 聚合才支持指定初始化器）
//   - StepByStep：auto w = au::Type(au::TypeProps{}); w.prop = val; 分步赋值形式
```

**关键约束：**

- 序列化格式（JSON）是 **规范形式**（canonical form），不含代码风格信息
- `to_code` 生成的代码 **必须可编译**，但不保证与原始代码结构相同
- 语义等价判定标准：`to_json(from_code(to_code(json))) == json`（往返一致性）
- 这是 **工具层**功能（CLI / MCP），不是库核心 API
- 与 #10 Inspector 集成：`au::to_code(au::dump_tree_json(root))` 可直接获取当前 UI 的代码表示
- 与 #17 CLI 集成：`aurora to-code tree.json --style fluent`
- `emit_props` 已扩展覆盖全部 15 种 `props_io` 属性类型（`bool`/`int`/`float`/`double`/`string`/`Color`/`Length`/
  `EdgeInsets`/枚举/`LocalizedString`/`Image`/`Alignment`/`FlexWeight`/`Flex`/`Json`）
- `InspectorPanel` 已支持导出代码：`export_code()` 方法 + “Export Code” 按钮 + `on_export_code` 回调，实现 Inspector→代码闭环

---
