# F1. AI 工具链层：Inspector / API Schema / 序列化（#10,#12,#13）

> 本文件是「三、特性详细规范」子文档，覆盖 **§F1.**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #10 内置 UI Inspector（WebSocket/MCP 接口）

**核心目标：** AI 可观测运行时

**规范：**

```cpp
// 任何时刻都可以导出 UI 树的结构化描述（JSON）
std::string dump = au::dump_tree_json(root).dump();   // 见 widget/inspect.h
// {
//   "type": "Column",
//   "children": [
//     {"type": "Text", "text": "Hello", "font_size": 24},
//     {"type": "Button", "text": "Click", "enabled": true}
//   ]
// }
// 人类可读缩进树：au::dump_tree(root)
// 富格式树（含 #id / bounds / visible / text / style / listeners + 树形符）：
//   au::dump_tree_rich(root)
```

**关键约束：**

- 运行时 Inspector 提供 **WebSocket 或 HTTP 服务接口**
- 不仅人类可用，AI Agent 也能通过 **MCP 等协议**查询组件树、属性、状态快照
- 形成"代码 → 运行 → 检查 → 修复"的反馈闭环
- **UI 树 dump 统一以 `widget/inspect.h` 内的自由函数提供**：`dump_tree`（缩进树）、`dump_tree_rich`（富格式，含 `#id`
  /几何/可见性/文本/样式/监听器）、`dump_tree_json*` / `dump_tree_json_full`（JSON 快照）。 **不提供 `Widget::dump()`
  成员方法**——富格式需求由 `dump_tree_rich` 覆盖，避免为每个控件重复实现 dump 逻辑，符合"最小核心 API"原则（#3）。

---

#### #12 机器可读 API Schema（JSON Schema）

**核心目标：** AI 工具链直接消费

**规范：**

随库发布结构化的 API 描述文件：

```json
{
  "component": "Button",
  "namespace": "aurora",
  "properties": [
    {
      "name": "label",
      "type": "LocalizedString",
      "required": true,
      "default": ""
    },
    {
      "name": "color",
      "type": "Color",
      "default": "blue",
      "note": "背景色（视觉变体靠背景色区分，无 variant 枚举）"
    },
    {
      "name": "on_color",
      "type": "Color",
      "default": "white",
      "note": "文字色"
    },
    {
      "name": "corner_radius",
      "type": "float",
      "default": "0"
    },
    {
      "name": "padding",
      "type": "EdgeInsets",
      "default": "12,6,12,6"
    },
    {
      "name": "enabled",
      "type": "bool",
      "default": "true"
    },
    {
      "name": "on_click",
      "type": "callback<void>",
      "required": false
    }
  ],
  "events": [
    "on_click",
    "on_hover",
    "on_focus"
  ],
  "children_policy": "none",
  "examples": [
    "au::Button(au::ButtonProps{ .label = \"Submit\" }).set_on_click(handle_submit)",
    "au::Button(au::ButtonProps{ .label = \"Cancel\", .color = au::colors::Gray })"
  ],
  "common_mistakes": [
    "Don't use Button.text() to read the label; use Button.label()"
  ]
}
```

**关键约束：**

- 格式为 JSON Schema / OpenAPI 风格接口描述
- 不仅包含函数签名，还要包含 **类型约束、默认值、组件组合规则**
- 提供 `aurora_api.json` 文件，供 LLM 直接作为 function calling 的 schema
- Schema 从代码 **自动生成**（通过宏/注解/编译插件），保证与实现同步

---

#### #13 UI 树序列化 + 差分 Patch 协议

**核心目标：** AI 可增量修改 UI

**规范：**

```cpp
// 任何 UI 树都可以双向转换
au::Json json = au::serialization::to_json(my_widget_tree);
auto restored = au::serialization::from_json(json);   // 返回 Result，使用前判 is_ok()

// YAML 输出（当前仅输出方向，无 from_yaml）
std::string yaml = au::serialization::to_yaml(my_widget_tree);  // Widget 树 → YAML
std::string yaml2 = au::serialization::to_yaml(json_value);      // Json → YAML

// 从结构化描述构建 UI
auto tree = au::serialization::from_json(au::Json::parse(R"({
    "type": "Column",
    "gap": 12,
    "children": [
        {"type": "Text", "text": "Hello World", "font_size": 24},
        {"type": "Button", "text": "Click Me"}
    ]
})");
```

**差分更新协议：**

```json
{
  "op": "patch",
  "changes": [
    {
      "path": "/children/1/text",
      "op": "replace",
      "value": "Updated"
    },
    {
      "path": "/children/2",
      "op": "add",
      "value": {
        "type": "Text",
        "text": "New"
      }
    },
    {
      "path": "/children/0",
      "op": "remove"
    }
  ]
}
```

允许 AI 只发送部分 UI 树 patch 而不是整个树，便于增量修改。

---
