# D. 布局与渲染层（#11,#20）

> 本文件是「三、特性详细规范」子文档，覆盖 **§D.**；完整章节导航（H 系列 + A–G 功能域）见 [SPECIFICATIONS.md](../../SPECIFICATIONS.md)。

#### #11 确定性渲染 + 逻辑快照测试

**核心目标：** AI 可验证正确性

**规范：**

- 相同输入 → **像素级相同输出**（跨平台）
- 提供 `au::render_to_png(root, width, height, path)` 离屏渲染为 PNG（见 §H.9）

**分层验证策略：**

```text
Level 1: 结构快照（JSON 树）—— AI 可完全验证
  {"type":"Column","children":[{"type":"Text","text":"Hi"}]}

Level 2: 布局盒模型快照 —— AI 可验证布局逻辑
  {"type":"Text","box":{"x":20,"y":10,"w":100,"h":24}}

Level 3: 像素快照（PNG）—— 人类视觉回归测试用
  仅在 CI 中由人类审查，AI 不直接消费
```

**快照测试：**

```cpp
static void test_button_renders_correctly() {
    auto btn = au::Button(au::ButtonProps{ .label = "Test" });
    btn.width(au::px(100)).height(au::px(40));   // 固定 100x40 逻辑像素
    au::Node root{ std::move(btn) };              // 包成 Node 树
    const au::Json snap = au::render_to_logical_snapshot(root, 100, 40);
    TCHECK(std::string{ snap["type"].get<std::string>() } == "Button");
    TCHECK(std::abs(snap["box"]["w"].get<float>() - 100.0f) < 0.001f);
    TCHECK(std::abs(snap["box"]["h"].get<float>() - 40.0f) < 0.001f);
}
```

**关键约束：** 快照格式是 **平台无关的逻辑描述**（JSON 树 + 布局盒模型），而不是像素位图，AI 可在无头环境验证跨平台 UI
结构是否正确。AI 的调试闭环只需要 Level 1 + Level 2，完全无头（headless）运行。

---

#### #20 布局系统的代数一致性

**核心目标：** AI 可推理尺寸和位置

**规范：**

```cpp
// 规则 1：盒模型完全显式，无隐式行为
auto hi = au::Text(au::TextProps{ .content = "Hi" });
hi.modifier = au::Modifier{}
    .padding(8)                   // 内边距，永远加在内容尺寸之外
    .border(1, au::colors::Gray)  // 边框
    .width(au::px(200));          // 内容宽度（强类型，必须带单位）
// 最终占用 = padding + border + width（无例外）

// 规则 2：百分比的参照物永远明确
child.width(au::percent(50));
// 参照物 = 父容器的 content width（不含 padding）
// 文档和 LSP 提示中必须写明参照物

// 规则 3：布局方程可求解、可验证
// 父容器宽度 = Σ(子宽度) + Σ(间距) + padding_left + padding_right
// 如果方程无解（子总宽 > 父宽），有明确的溢出策略：
// 溢出策略：用 Scroll 容器包裹，或依赖约束 clamp
au::Scroll(au::ScrollProps{ .child = au::Row(au::RowProps{ .children = { /* ... */ } }) });  // 可滚动（Scroll 为单子容器）
// 布局方程无解时按约束 clamp（永不未定义行为）

// 规则 4：布局结果可查询
auto snap = au::render_to_logical_snapshot(root, 800, 600);   // 3 个 int 参数（见 include/aurora/render/offscreen.h）
// snap 内含每个组件的确定性盒模型：{x:20, y:10, w:100, h:24}
```

**关键约束：**

- 布局模型基于线性等式（如 Flexbox），具有明确可计算的盒模型
- AI 能通过简单规则推导：父宽度 = 子宽度之和 + 间距
- **无隐藏的边距合并**等怪异行为
- 无隐式最小尺寸

**动态/响应式布局规则：**

```cpp
// 规则 5：窗口 resize 时的布局重计算
// 布局是纯函数：layout(tree, viewport_size) → boxes
// 窗口大小变化 → 重新调用 render_to_logical_snapshot → 确定性结果
// 无动画插值：布局跳变是即时的，动画仅作用于视觉属性（opacity, transform）

// 规则 6：动态内容（如文本换行）的处理
// 文本组件的 height 默认为 auto（由内容决定）
// 布局分两遍：
//   Pass 1: 确定宽度（自上而下）
//   Pass 2: 确定高度（自下而上，文本换行后确定实际高度）
// 两遍布局保证确定性，AI 可推理最终结果

// 规则 7：动画/过渡期间布局不变
// 动画仅影响渲染层的 transform/opacity，不改变布局盒模型
// 布局快照在动画前后完全一致
```

---
