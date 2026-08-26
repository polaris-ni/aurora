# H.12 序列化与结构快照

> 本文件是「三、特性详细规范」子文档，覆盖 **§H.12**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #H.12 序列化与结构快照（Diff / Patch）

核心目标：UI 树可双向转换与增量修改，供 AI 工具链消费（§13）。

- **`serialization::to_json(widget)` / `serialization::from_json(json)`**：UI 树 ↔ JSON（结构 + 属性）；`from_json` 返回
  `Result`，失败经 `Result::Error` 表达，使用前须 `is_ok()`。组件通过 `serialize_props` / `deserialize_props` 虚钩子声明可序列化属性。
- **`serialization::diff(before, after)` / `diff_into` / `serialization::apply_patch`**：生成 / 应用 JSON Patch（见 §13
  差分协议）；`apply_patch` 第一参数是 `au::Json`（ **非** widget 树），AI 可只发部分 UI 树 patch 做增量修改。
- **`WidgetRegistry`**：组件工厂注册表。`WidgetRegistry::make(type_name, props)` 按类型名构造组件；`list_types()`
  枚举所有已注册类型。Schema 可从注册表自动生成（§12）。
- **`Scene::serialize`**：把运行中 UI 导出为结构化描述（返回 `std::string`，无参）；对应规格 §10 Inspector。

```cpp
au::Json json = au::serialization::to_json(my_tree);
auto restored = au::serialization::from_json(json);   // 返回 Result，使用前判 is_ok()
auto patch = au::serialization::diff(before_json, after_json);   // [{op:"replace", path:"/children/1/label", value:"Updated"}]
au::serialization::apply_patch(json, patch);   // 第一参数是 au::Json（非 widget 树）
```
